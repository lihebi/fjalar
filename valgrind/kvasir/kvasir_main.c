/*
   This file is part of Kvasir, a Valgrind skin that implements the
   C language front-end for the Daikon Invariant Detection System

   Copyright (C) 2004-2005 Philip Guo, MIT CSAIL Program Analysis Group

   2005-04-28:
   Ported over to Valgrind 3 and integrated with the DynComp dynamic
   comparability analysis tool for C/C++.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

/* kvasir_main.c:

   This file contains most of the code to interact with the Valgrind
   core.  It is called from mc_main.c since mc_main.c is the
   launching-point for Kvasir.
*/

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "tool.h"
#include "generate_daikon_data.h"
#include "kvasir_main.h"
#include "decls-output.h"
#include "dtrace-output.h"
#include "mc_include.h"
#include "disambig.h"

// Global variables that are set by command-line options
char* kvasir_decls_filename = 0;
char* kvasir_dtrace_filename = 0;
Bool kvasir_with_dyncomp = False;
Bool kvasir_print_debug_info = False;
Bool kvasir_ignore_globals = False;
Bool kvasir_ignore_static_vars = False;
Bool kvasir_dtrace_append = False;
Bool kvasir_dtrace_gzip = False;
Bool kvasir_output_fifo = False;
#ifdef KVASIR_DEVEL_BUILD
Bool kvasir_asserts_aborts_on = True;
#else
Bool kvasir_asserts_aborts_on = False;
#endif
Bool kvasir_decls_only = False;
Bool kvasir_limit_static_vars = False;
Bool kvasir_default_disambig = False;
Bool kvasir_use_bit_level_precision = False;
int kvasir_array_length_limit = -1;
char* kvasir_dump_prog_pt_names_filename = 0;
char* kvasir_dump_var_names_filename = 0;
char* kvasir_trace_prog_pts_filename = 0;
char* kvasir_trace_vars_filename = 0;
char* kvasir_disambig_filename = 0;
char *kvasir_program_stdout_filename = 0;
char *kvasir_program_stderr_filename = 0;

Bool actually_output_separate_decls_dtrace = 0;

/*------------------------------------------------------------*/
/*--- Function stack                                       ---*/
/*------------------------------------------------------------*/
// TODO: Should I MAKE THIS HUGE??? or dynamically-allocate??? (original = 10000)
#define FN_STACK_SIZE 10000

FunctionEntry fn_stack[FN_STACK_SIZE];
Int fn_stack_top;       // actually just above top -- next free slot

void printFunctionEntryStack()
{
   int i;
   for (i = fn_stack_top - 1; i >= 0; i--)
      {
         FunctionEntry* cur_fn = &fn_stack[i];
         VG_(printf)("fn_stack[%d] %s - EBP: 0x%x, lowestESP: 0x%x, localArrayVarPtr: %p\n",
                i,
                cur_fn->name,
                cur_fn->EBP,
                cur_fn->lowestESP,
                cur_fn->localArrayVariablesPtr);
      }
}

/*------------------------------------------------------------*/
/*--- Function entry/exit                                  ---*/
/*------------------------------------------------------------*/

/*
Requires:
Modifies: fn_stack, fn_stack_top
Returns:
Effects: pushes a FunctionEntry onto the top of fn_stack and initializes
         it with function name (f), and EBP values.
         This is called during function entrance.  Initializes
         "virtual stack" and then calls handleFunctionEntrance() to
         generate .dtrace file output at function entrance time
*/
static void push_fn(Char* s, Char* f, Addr EBP, Addr startPC)
{
  DaikonFunctionInfo* daikonFuncPtr =
    findFunctionInfoByAddr(startPC);

  int formalParamStackByteSize =
     determineFormalParametersStackByteSize(daikonFuncPtr);

  FunctionEntry* top;

  DPRINTF("formalParamStackByteSize is %d\n", formalParamStackByteSize);

  if (fn_stack_top >= FN_STACK_SIZE) VG_(tool_panic)("overflowed fn_stack");

  top = &fn_stack[ fn_stack_top ];

  top->name = f;
  top->EBP = EBP;
  top->startPC = startPC;
  top->lowestESP = EBP + 4;
  top->EAX = 0;
  top->EDX = 0;
  top->FPU = 0;

  // Initialize virtual stack and copy parts of the Valgrind stack
  // into that virtual stack
  if (formalParamStackByteSize > 0) {
     top->virtualStack = VG_(calloc)(formalParamStackByteSize, sizeof(char));
     top->virtualStackByteSize = formalParamStackByteSize;

     VG_(memcpy)(top->virtualStack, (void*)EBP, formalParamStackByteSize);
     // VERY IMPORTANT!!! Copy all the A & V bits over from EBP to virtualStack!!!
     mc_copy_address_range_state(EBP, (Addr)(top->virtualStack), formalParamStackByteSize);
  }
  else {
     // Watch out for null pointer segfaults here:
     top->virtualStack = 0;
     top->virtualStackByteSize = 0;
  }

  // Initialize the FunctionEntry.localArrayVariablesPtr field:
  top->localArrayVariablesPtr = &(daikonFuncPtr->localArrayVariables);

  fn_stack_top++;

  // We used to do this BEFORE the push - does it make a difference???
  DPRINTF("-- PUSH_FN: fn_stack_top: %d, f: %s\n", fn_stack_top, f);

  // Do this AFTER initializing virtual stack and lowestESP
  handleFunctionEntrance(top);
}

/*
Requires:
Modifies: fn_stack, fn_stack_top
Returns:
Effects: pops a FunctionEntry off of the top of fn_stack and initializes
         it with EAX, EDX, and FPU values. Then calls handleFunctionExit()
         to generate .dtrace file output at function exit time
*/
static void pop_fn(Char* s,
                   int EAX, int EDX, double FPU_top,
                   UInt EAXshadow, UInt EDXshadow, ULong FPUshadow)
{
   FunctionEntry* top;
   int i;

   // s is null if an "unwind" is popped off the stack
   // Only do something if this function name matches what's on the top of the stack
   if (!s || (!VG_STREQ(fn_stack[fn_stack_top - 1].name, s))) {
      return;
   }

  if (fn_stack_top < 1) VG_(tool_panic)("underflowed fn_stack");

  top =  &fn_stack[ fn_stack_top - 1 ];

  top->EAX = EAX;
  top->EDX = EDX;
  top->FPU = FPU_top;

  // Very important!  Set the A and V bits of the appropriate
  // FunctionEntry object:

  for (i = 0; i < 4; i++) {
     set_abit((Addr)(&(top->EAX)) + (Addr)i, VGM_BIT_VALID);
     set_abit((Addr)(&(top->EDX)) + (Addr)i, VGM_BIT_VALID);
     set_abit((Addr)(&(top->FPU)) + (Addr)i, VGM_BIT_VALID);

     set_vbyte((Addr)(&(top->EAX)) + (Addr)i, (UChar)((EAXshadow & 0xff) << (i * 8)));
     set_vbyte((Addr)(&(top->EDX)) + (Addr)i, (UChar)((EDXshadow & 0xff) << (i * 8)));
     set_vbyte((Addr)(&(top->FPU)) + (Addr)i, (UChar)((FPUshadow & 0xff) << (i * 8)));
  }

  for (i = 4; i < 8; i++) {
     set_abit((Addr)(&(top->FPU)) + (Addr)i, VGM_BIT_VALID);

     set_vbyte((Addr)(&(top->FPU)) + (Addr)i, (UChar)((FPUshadow & 0xff) << (i * 8)));
  }

  DPRINTF("------ POP_FN: fn_stack_top: %d, s: %s\n", fn_stack_top, s);

  handleFunctionExit(top);

   // Destroy the memory allocated by virtualStack
   if (top->virtualStack) {
      VG_(free)(top->virtualStack);
   }

   fn_stack_top--; // Now pop it off by decrementing fn_stack_top
}


/*
Requires:
Modifies: fn_stack, fn_stack_top
Returns:
Effects: This is the hook into Valgrind that is called whenever the target
         program enters a function.  Calls push_fn() if all goes well.
*/
// Rudimentary function entrance/exit tracking
VGA_REGPARM(2)
void enter_function(Char* fnname, Addr StartPC)
{
   Addr  ESP = VG_(get_SP)(VG_(get_running_tid)());
   // Assign %esp - 4 to %ebp - empirically tested to be
   // correct for calling conventions
   Addr  EBP = ESP - 4;

   VG_(printf)("Enter function: %s - StartPC: %p\n",
	   fnname, (void*)StartPC);

   DPRINTF("Calling push_fn for %s\n", fnname);

   push_fn("call", fnname, EBP, StartPC);
}

/*
Requires:
Modifies: fn_stack, fn_stack_top, topEntry
Returns:
Effects: This is the hook into Valgrind that is called whenever the target
         program exits a function.  Initializes topEntry of fn_stack with
         return values from EAX, EDX, and FPU.  Calls pop_fn() if all goes well.
*/
VGA_REGPARM(1)
void exit_function(Char* fnname)
{
   ThreadId currentTID = VG_(get_running_tid)();

   // Get the value at the simulated %EAX (integer and pointer return
   // values are stored here upon function exit)
   Addr EAX = VG_(get_EAX)(currentTID);

   // Get the value of the simulated %EDX (the high 32-bits of the
   // long long int return value is stored here upon function exit)
   Addr EDX = VG_(get_EDX)(currentTID);

   // Ok, in Valgrind 2.X, we needed to directly code some assembly to grab
   // the top of the floating-point stack, but Valgrind 3.0 provides a virtual
   // FPU stack, so we can just grab that.  Plus, we now have shadow V-bits
   // for the FPU stack.
   double fpuReturnVal = VG_(get_FPU_stack_top)(currentTID);

   // 64 bits
   // Use SHADOW values of Valgrind simulated registers to get V-bits
   UInt EAXshadow = VG_(get_shadow_EAX)(currentTID);
   UInt EDXshadow = VG_(get_shadow_EDX)(currentTID);
   ULong FPUshadow = VG_(get_shadow_FPU_stack_top)(currentTID);

   VG_(printf)("Exit function: %s - EAX: 0x%x, 0x%x, 0x%x\n",
               fnname, EAX, EAXshadow, EDXshadow);

   pop_fn(fnname, EAX, EDX, fpuReturnVal, EAXshadow, EDXshadow, FPUshadow);
}


/*
Requires:
Modifies: lots of global stuff
Returns:
Effects: All of the Kvasir initialization is performed right here;
         Memcheck mc_main.c calls this at the end of its own
         initialization and this must extract DWARF2 debugging
         information from the ELF executable, process the
         dwarf_entry_array, and create .decls and .dtrace files
*/

// Before processing command-line options
void kvasir_pre_clo_init()
{
   // Clear fn_stack
   VG_(memset)(fn_stack, 0, FN_STACK_SIZE * sizeof(*fn_stack));

   // Clear all global variables before processing command-line options:
   kvasir_decls_filename = 0;
   kvasir_dtrace_filename = 0;
   kvasir_print_debug_info = False;
   kvasir_ignore_globals = False;
   kvasir_ignore_static_vars = False;
   kvasir_dtrace_append = False;
   kvasir_dtrace_gzip = False;
   kvasir_output_fifo = False;

#ifdef KVASIR_DEVEL_BUILD
   kvasir_asserts_aborts_on = True;
#else
   kvasir_asserts_aborts_on = False;
#endif

   kvasir_decls_only = False;
   kvasir_limit_static_vars = False;
   kvasir_default_disambig = False;
   kvasir_dump_prog_pt_names_filename = 0;
   kvasir_dump_var_names_filename = 0;
   kvasir_trace_prog_pts_filename = 0;
   kvasir_trace_vars_filename = 0;
   kvasir_disambig_filename = 0;
   kvasir_program_stdout_filename = 0;
   kvasir_program_stderr_filename = 0;
}

// Initialize kvasir after processing command-line options
void kvasir_post_clo_init()
{
  // Assume that the filename is the FIRST string in
  // VG(client_argv) since that is the client argv array
  // after being parsed by the Valgrind core:
  char* filename = VG_(client_argv)[0];

  // Handle variables set by command-line options:
  char* DISAMBIG = ".disambig";
  int DISAMBIG_LEN = VG_(strlen)(DISAMBIG);

  DPRINTF("\nReading binary file \"%s\" [0x%x] (Assumes that filename is first argument in client_argv)\n\n",
	  filename, filename);
  DPRINTF("handleFunctionEntrance is at %p\n", handleFunctionEntrance);

  // --disambig results in the disambig filename being ${filename}.disambig
  // (overrides --disambig-file option)
  if (kvasir_default_disambig) {
    char* disambig_filename =
      VG_(calloc)(VG_(strlen)(filename) + DISAMBIG_LEN + 1,
	     sizeof(*disambig_filename));

    VG_(strcpy)(disambig_filename, filename);
    VG_(strcat)(disambig_filename, DISAMBIG);
    kvasir_disambig_filename = disambig_filename;
  }
  // --disambig-file=F results in the disambig filename being ${F}.disambig
  else if (kvasir_disambig_filename) {
    char* disambig_filename =
      VG_(calloc)(VG_(strlen)(kvasir_disambig_filename) + DISAMBIG_LEN + 1,
	     sizeof(*disambig_filename));

    VG_(strcpy)(disambig_filename, kvasir_disambig_filename);
    VG_(strcat)(disambig_filename, DISAMBIG);
    kvasir_disambig_filename = disambig_filename;
  }

  DPRINTF("\n%s\n\n", kvasir_disambig_filename);

  // Special-case .dtrace handling if kvasir_dtrace_filename ends in ".gz"
  if (kvasir_dtrace_filename) {
    int filename_len = VG_(strlen)(kvasir_dtrace_filename);
    if VG_STREQ(kvasir_dtrace_filename + filename_len - 3, ".gz") {
      DPRINTF("\nFilename ends in .gz\n");
      // Chop off '.gz' from the end of the filename
      kvasir_dtrace_filename[filename_len - 3] = '\0';
      // Activate kvasir_dtrace_gzip
      kvasir_dtrace_gzip = True;
    }
  }

  // Output separate .decls and .dtrace files if:
  // --decls-only is on OR --decls-file=<filename> is on
  if (kvasir_decls_only || kvasir_decls_filename) {
    DPRINTF("\nSeparate .decls\n\n");
    actually_output_separate_decls_dtrace = True;
  }

  process_elf_binary_data(filename);
  daikon_preprocess_entry_array();
  createDeclsAndDtraceFiles(filename);
}

void kvasir_print_usage()
{
   VG_(printf)(
"    --with-dyncomp      enables DynComp comparability analysis [--no-dyncomp]\n"
"    --debug             print Kvasir-internal debug messages [--no-debug]\n"
#ifdef KVASIR_DEVEL_BUILD
"    --asserts-aborts    turn on safety asserts and aborts (ON BY DEFAULT)\n"
"                        [--asserts-aborts]\n"
#else
"    --asserts-aborts    turn on safety asserts and aborts (OFF BY DEFAULT)\n"
"                        [--no-asserts-aborts]\n"
#endif
"    --ignore-globals     ignores all global variables [--no-ignore-globals]\n"
"    --ignore-static-vars ignores all static variables [--no-ignore-static-vars]\n"
"    --limit-static-vars  limits the output of static vars [--no-limit-static-vars]\n"
"    --bit-level-precision     Uses bit-level precision to produce more accurate\n"
"                              output at the expense of speed [--no-bit-level-precision]\n"
"    --nesting-depth=N   limits the maximum number of dereferences of any structure\n"
"                        to N [--nesting-depth=2]\n"
"                        (N must be an integer between 0 and 100)\n"
"    --struct-depth=N    limits the maximum number of dereferences of recursively\n"
"                        defined structures (i.e. linked lists) to N [--struct-depth=2]\n"
"                        (N must be an integer between 0 and 100)\n"
"    --dtrace-append     appends .dtrace data to the end of the existing file\n"
"                        [--no-dtrace-append]\n"
"    --output-fifo       create output files as named pipes [--no-output-fifo]\n"
"    --decls-only        exit after creating .decls file [--no-decls-only]\n"
"    --decls-file=<string>    the output .decls file location\n"
"                             [daikon-output/FILENAME.decls]\n"
"                             (forces generation of separate .decls file)\n"
"    --dtrace-file=<string>   the output .dtrace file location\n"
"                             [daikon-output/FILENAME.dtrace]\n"
"    --dtrace-gzip            compresses .dtrace data [--no-dtrace-gzip]\n"
"                             (Automatically ON if --dtrace-file string ends in '.gz')\n"
"    --dump-ppt-file=<string> outputs all program point names to a file\n"
"    --dump-var-file=<string> outputs all variable names to a file\n"
"    --ppt-list-file=<string> trace only the program points listed in this file\n"
"    --var-list-file=<string> trace only the variables listed in this file\n"
"    --disambig-file=<string> Reads in disambig file if exists; otherwise creates one\n"
"    --disambig               Uses <program name>.disambig as the disambig file\n"
"    --program-stdout=<file>  redirect instrumented program stdout to file\n"
"                             [Kvasir's stdout, or /dev/tty if --dtrace-file=-]\n"
"    --program-stderr=<file>  redirect instrumented program stderr to file\n"
   );
}

/* Like VG_BOOL_CLO, but of the form "--foo", "--no-foo" rather than
   "--foo=yes", "--foo=no". Note that qq_option should not have a
   leading "--". */

#define VG_YESNO_CLO(qq_option, qq_var) \
   if (VG_CLO_STREQ(arg, "--"qq_option)) { (qq_var) = True; } \
   else if (VG_CLO_STREQ(arg, "--no-"qq_option))  { (qq_var) = False; }

// Processes command-line options
Bool kvasir_process_cmd_line_option(Char* arg)
{
   VG_STR_CLO(arg, "--decls-file", kvasir_decls_filename)
   else VG_STR_CLO(arg, "--dtrace-file",    kvasir_dtrace_filename)
   else VG_YESNO_CLO("with-dyncomp",   kvasir_with_dyncomp)
   else VG_YESNO_CLO("debug",          kvasir_print_debug_info)
   else VG_YESNO_CLO("ignore-globals", kvasir_ignore_globals)
   else VG_YESNO_CLO("ignore-static-vars", kvasir_ignore_static_vars)
   else VG_YESNO_CLO("dtrace-append",  kvasir_dtrace_append)
   else VG_YESNO_CLO("dtrace-gzip",    kvasir_dtrace_gzip)
   else VG_YESNO_CLO("output-fifo",    kvasir_output_fifo)
   else VG_YESNO_CLO("asserts-aborts", kvasir_asserts_aborts_on)
   else VG_YESNO_CLO("decls-only",     kvasir_decls_only)
   else VG_YESNO_CLO("limit-static-vars", kvasir_limit_static_vars)
   else VG_YESNO_CLO("bit-level-precision", kvasir_use_bit_level_precision)
   else VG_BNUM_CLO(arg, "--struct-depth",  MAX_STRUCT_INSTANCES, 0, 100) // [0 to 100]
   else VG_BNUM_CLO(arg, "--nesting-depth", MAX_NUM_STRUCTS_TO_DEREFERENCE, 0, 100) // [0 to 100]
   else VG_BNUM_CLO(arg, "--array-length-limit", kvasir_array_length_limit,
                    -1, 0x7fffffff)
   else VG_YESNO_CLO("disambig", kvasir_default_disambig)
   else VG_STR_CLO(arg, "--dump-ppt-file",  kvasir_dump_prog_pt_names_filename)
   else VG_STR_CLO(arg, "--dump-var-file",  kvasir_dump_var_names_filename)
   else VG_STR_CLO(arg, "--ppt-list-file",  kvasir_trace_prog_pts_filename)
   else VG_STR_CLO(arg, "--var-list-file",  kvasir_trace_vars_filename)
   else VG_STR_CLO(arg, "--disambig-file",  kvasir_disambig_filename)
   else VG_STR_CLO(arg, "--program-stdout", kvasir_program_stdout_filename)
   else VG_STR_CLO(arg, "--program-stderr", kvasir_program_stderr_filename)
   else
      return MAC_(process_common_cmd_line_option)(arg);

  return True;
}


// This runs after Kvasir finishes
void kvasir_finish() {

  if (disambig_writing) {
    generateDisambigFile();
  }

  finishDtraceFile();
}
