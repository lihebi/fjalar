/*
   This file is part of Kvasir, a Valgrind skin that implements the
   C language front-end for the Daikon Invariant Detection System

   Copyright (C) 2004 Philip Guo, MIT CSAIL Program Analysis Group

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

/* dtrace-output.c:
   Functions for outputting Kvasir runtime variable values
   to a Daikon-compatible .dtrace file
*/

#define _FILE_OFFSET_BITS 64
#define _LARGEFILE64_SOURCE /* FOR O_LARGEFILE */

#include "dtrace-output.h"
#include "decls-output.h"
#include "kvasir_main.h"
#include "dyncomp_main.h"
#include "dyncomp_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "mc_include.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) < (b) ? (a) : (b))


#define DTRACE_PRINTF(...) do { if (!dyncomp_without_dtrace) \
      fprintf(dtrace_fp, __VA_ARGS__); } while (0)


char* UNINIT = "uninit";
char* NONSENSICAL = "nonsensical";

// This provides a 8-byte temporary storage area for single variables.
// (This is large enough for GNU 'long long ints', I think
// This is used to store the results of masking the program's variables
// with V-bit bit-masks because we don't want to alter the program's
// variables.
char GLOBAL_TEMP_VAR[8] = {0, 0, 0, 0, 0, 0, 0, 0};
// The global 8-byte mask:
char GLOBAL_MASK[8] = {0, 0, 0, 0, 0, 0, 0, 0};

// The indices to this array must match the DaikonDeclaredType enum
// declared in generate_daikon_data.h:
static const char* TYPE_FORMAT_STRINGS[ ] = {
  "%d - ERROR - D_NO_TYPE", //     D_NO_TYPE, // Create padding
  "%u",   //     D_UNSIGNED_CHAR,
  "%d",   //     D_CHAR
  "%hu",  //     D_UNSIGNED_SHORT,
  "%hd",  //     D_SHORT,
  "%u",   //     D_UNSIGNED_INT,
  "%d",   //     D_INT,
  "%llu", //     D_UNSIGNED_LONG_LONG_INT,
  "%lld", //     D_LONG_LONG_INT,
  "%.9g", //     D_UNSIGNED_FLOAT, // currently unused
  "%.9g", //     D_FLOAT,
  "%.17g",//     D_UNSIGNED_DOUBLE, // currently unused
  "%.17g",//     D_DOUBLE,

  "%.17g",//     D_UNSIGNED_LONG_DOUBLE, // currently unused
  "%.17g",//     D_LONG_DOUBLE, // currently unused

  "%d",   //     D_ENUMERATION,

  "%d - ERROR - D_STRUCT", //     D_STRUCT, // currently unused
  "%d - ERROR - D_UNION", //     D_UNION, // currently unused
  "%d - ERROR - D_FUNCTION", //     D_FUNCTION // currently unused
  "%d - ERROR - D_VOID", //     D_VOID // currently unused

  "%d - ERROR - D_CHAR_AS_STRING",    //     D_CHAR_AS_STRING // currently unused
  "%d" ,  //     D_BOOL
};

// The indices to this array must match the DaikonDeclaredType enum
// declared in generate_daikon_data.h:
static const int TYPE_BYTE_SIZES[ ] = {
  sizeof(char), //     D_NO_TYPE, // Create padding
  sizeof(unsigned char),   //     D_UNSIGNED_CHAR,
  sizeof(char),   //     D_CHAR,
  sizeof(unsigned short),  //     D_UNSIGNED_SHORT,
  sizeof(short),  //     D_SHORT,
  sizeof(unsigned int),   //     D_UNSIGNED_INT,
  sizeof(int),   //     D_INT,
  sizeof(unsigned long long int), //     D_UNSIGNED_LONG_LONG_INT,
  sizeof(long long int), //     D_LONG_LONG_INT,
  sizeof(float), //     D_UNSIGNED_FLOAT, // currently unused
  sizeof(float), //     D_FLOAT,
  sizeof(double),//     D_UNSIGNED_DOUBLE, // currently unused
  sizeof(double),//     D_DOUBLE,

  sizeof(char), //     D_UNSIGNED_LONG_DOUBLE, // currently unused
  sizeof(char), //     D_LONG_DOUBLE, // currently unused

  sizeof(int),   //     D_ENUMERATION,

  sizeof(char), //     D_STRUCT, // currently unused
  sizeof(char), //     D_UNION, // currently unused
  sizeof(char), //     D_FUNCTION, // currently unused
  sizeof(char), //     D_VOID // currently unused

  sizeof(char), //     D_CHAR_AS_STRING
  sizeof(char), //     D_BOOL
};

extern FILE* dtrace_fp; // This should be initialized in decls-output.c

void printDtraceFunctionHeader(DaikonFunctionInfo* funcPtr, char isEnter)
{
  DPRINTF("Printing dtrace header for %s\n", funcPtr->daikon_name);
  DPRINTF("dtrace_fp is %p\n", dtrace_fp);
  tl_assert(dtrace_fp);
  fputs("\n", dtrace_fp);
  fputs(funcPtr->daikon_name, dtrace_fp);

  if (isEnter)
    {
      fputs(ENTER_PPT, dtrace_fp);
      fputs("\n", dtrace_fp);
    }
  else
    {
      fputs(EXIT_PPT, dtrace_fp);
      fputs("\n", dtrace_fp);
    }
  DPRINTF("Done printing header for %s\n", funcPtr->daikon_name);
}

// Maps init value to modbit
// init = 1 ---> modbit = 1
// init = 0 ---> modbit = 2
char mapInitToModbit(char init)
{
  if (init)
    {
      return 1; // Make it seem like it's "modified" by default
    }
  else
    {
      return 2; // Garbage value
    }
}

// Returns 1 if both allocated and initialized (safe to dereference);
// otherwise returns 0:
static char checkAllocAndInit(void* ptrValue) {
  char allocated = addressIsAllocated((Addr)ptrValue, sizeof(void*));
  char initialized = addressIsInitialized((Addr)ptrValue, sizeof(void*));
  return (allocated && initialized);
}

// Checks whether the contents of ptrValue are allocated and
// initialized.  Returns 1 if allocated and initialized.  Otherwise
// returns 0 and prints an error line to .dtrace:
static char checkAllocAndInitPrintMsg(void* ptrValue, char overrideIsInitialized) {
  char allocated = (overrideIsInitialized ? 1 :
                    addressIsAllocated((Addr)ptrValue, sizeof(void*)));

  char initialized = 0;

  if (!allocated) {
    DTRACE_PRINTF( "%s\n%d\n",
                   NONSENSICAL,
                   mapInitToModbit(0));
    return 0;
  }

  initialized = (overrideIsInitialized ? 1 :
                 addressIsInitialized((Addr)ptrValue, sizeof(void*)));

  if (!initialized) {
    DTRACE_PRINTF( "%s\n%d\n",
                   UNINIT,
                   mapInitToModbit(0));
    return 0;
  }

  return 1;
}


// Prints a string to dtrace_fp, keeping in mind to quote
// special characters so that the lines don't get screwed up
void printOneDtraceString(char* str)
{
  char readable;
  Addr strHead = (Addr)str;
  int len = 0;
  // Print leading and trailing quotes to "QUOTE" the string
  DTRACE_PRINTF("\"");
  readable = addressIsInitialized((Addr)str, sizeof(char));
  tl_assert(readable);
  while (*str != '\0')
    {
      switch (*str) {
      case '\n':
        DTRACE_PRINTF( "\\n");
        break;
      case '\r':
        DTRACE_PRINTF( "\\r");
        break;
      case '\"':
        DTRACE_PRINTF( "\\\"");
        break;
      case '\\':
        DTRACE_PRINTF( "\\\\");
        break;
      default:
        DTRACE_PRINTF( "%c", *str);
      }

      str++;
      len++;

      readable = addressIsInitialized((Addr)str, sizeof(char));

      if (!readable) {
        DPRINTF("  whoa, ran into unreadable character\n");
	DABORT("unreadable character in printOneDtraceString");
        break;
      }
    }
  DTRACE_PRINTF("\"");

  // We know the length of the string so merge the tags
  // for that many contiguous bytes in memory
  if (kvasir_with_dyncomp) {
    DYNCOMP_DPRINTF("dtrace call val_uf_union_tags_in_range(0x%x, %d)\n",
                    (Addr)strHead, len);
    val_uf_union_tags_in_range(strHead, len);
  }
}

// Prints one character as though it were a string to .dtrace,
// making sure to not mess up the line format
void printOneCharAsDtraceString(char c)
{
  // Print leading and trailing quotes to "QUOTE" the string
  DTRACE_PRINTF( "\"");

  switch (c) {
  case '\n':
    DTRACE_PRINTF( "\\n");
    break;
  case '\r':
    DTRACE_PRINTF( "\\r");
    break;
  case '\"':
    DTRACE_PRINTF( "\\\"");
    break;
  case '\\':
    DTRACE_PRINTF( "\\\\");
    break;
  default:
    DTRACE_PRINTF( "%c", c);
  }

  DTRACE_PRINTF( "\"");
}

void printOneDtraceStringAsIntArray(char* str) {
  char readable;
  Addr strHead = (Addr)str;
  int len = 0;

  DTRACE_PRINTF("[ ");
  readable = addressIsInitialized((Addr)str, sizeof(char));
  tl_assert(readable);
  while (*str != '\0')
    {
      DTRACE_PRINTF( "%d ", *str);

      str++;
      len++;

      readable = addressIsInitialized((Addr)str, sizeof(char));
      if (!readable) {
        DPRINTF("  whoa, ran into unreadable character\n");
	DABORT("unreadable character in printOneDtraceStringAsIntArray");
        break;
      }
    }
  DTRACE_PRINTF("]");


  // We know the length of the string so merge the tags
  // for that many contiguous bytes in memory
  if (kvasir_with_dyncomp) {
    DYNCOMP_DPRINTF("dtrace call val_uf_union_tags_in_range(0x%x, %d)\n",
                    (Addr)strHead, len);
    val_uf_union_tags_in_range(strHead, len);
  }
}

/* Returns true if str points to a null-terminated string, every byte of
   which up to the \0 is readable according to memcheck.
*/
static int checkStringReadable(char *str) {
  char *p = str;
  for (;;) {
    int readable = addressIsInitialized((Addr)p, sizeof(char));
    if (!readable) {
      DPRINTF("String contains unreadable byte %d (%p)\n", p - str, p);
      return 0;
    }
    else if (!*p) {
      DPRINTF("All %d string characters are readable (%p)\n", p - str, p);
      return 1;
    }
    p++;
  }
}

/*
Requires: cur_node is the node to print, basePtrValue is the pointer
          that points to the value to print out

Properly outputs the following basic types:
Output as numbers: D_UNSIGNED_CHAR,D_UNSIGNED_SHORT, D_SHORT,
D_UNSIGNED_INT, D_INT, D_UNSIGNED_LONG_LONG_INT, D_LONG_LONG_INT,
D_UNSIGNED_FLOAT, D_FLOAT, D_UNSIGNED_DOUBLE, D_DOUBLE, D_ENUMERATION
Output as address: pointer
Output as alphanumeric characters: R_STRING, D_CHAR

If overrideIsInitialized != 0, then the variable is automatically
counted as allocated AND initialized - THIS IS VERY DANGEROUS and should be activated JUDICIOUSLY!

If isDummy = 1, print out "NONSENSICAL" and modbit=2

Support for sequence printing:
isSequence - are we printing a sequence?
upperBound - the maximum # of elts in the array
sequenceEltsToOutput - array of pointers to contents to output

If isSequence == true, then it will print out consecutive values as
follows (all enclosed by brackets):

*(sequenceEltsToOutput[0])
*(sequenceEltsToOutput[1])
*(sequenceEltsToOutput[2])
...
*(sequenceEltsToOutput[upperBound])

If isSequence == false, then it will output one value corresponding to:
*basePtrValue

if isHashcode, then we are DEFINITELY printing out a pointer

Pre: basePtrValue IS INITIALIZED AND OKAY TO DEREFERENCE!!!
     *basePtrValue is what we are printing out so it better be ok to dereference!!!

basePtrValue contains the address of the variable we are trying to print out
so we will actually print *basePtrValue (or sequenceEltsToOutput)
Returns: 1 if the value of the variable was actually observed and outputted
         (ie. it was a valid and initialized value),
         0 if the variable was never observed and either UNINIT or NONSENSICAL was printed out
*/
char outputDtraceValue(DaikonVariable* var,
		       void* basePtrValue,
		       VariableOrigin varOrigin,
		       char isHashcode,
		       char overrideIsInitialized,
		       char isDummy, // automatically print out NONSENSICAL
		       char isSequence,
		       unsigned long upperBound,
                       void** sequenceEltsToOutput,
		       char overrideFloatAsDouble,
		       DisambigOverride disambigOverride)
{
  void* ptrValue = basePtrValue;

  int i = 0;

  // Default this to 1 unless otherwise changed
  char variableHasBeenObserved = 1;

  if (!var) {
    VG_(printf)( "outputDtraceValue() - var is null\n");
    abort();
  }

  if (isDummy) {
    DTRACE_PRINTF( "%s\n%d\n",
                   NONSENSICAL,
                   mapInitToModbit(0));

    return 0;
  }

  // Pointer (put this check first before the var->isString check
  //          so that it will work even for pointers to strings):
  if (isHashcode) {
    // TODO: We need to check if the bytes are actually INITIALIZED
    // to some useful value, not merely that it's been ALLOCATED
    // (this is for the modbit)
    // (Don't we not support initialization bits right now?  Our modbit
    //  simply indicates whether it's been allocated or not, right?)
    if (isSequence) {
      printDtraceHashcodeSequence(var, sequenceEltsToOutput, upperBound);
    }
    else {
      checkAllocAndInitPrintMsg(ptrValue, overrideIsInitialized);
      variableHasBeenObserved = printDtraceSingleHashcode(var, ptrValue);
    }
  }
  else if (var->isString) {
    if (isSequence) {
      printDtraceStringSequence(var,
                                disambigOverride,
                                sequenceEltsToOutput,
                                upperBound);
    }
    else {
      char allocatedAndReadable = 0;

      // Depends on whether the variable is a static array or not:
      Addr addressInQuestion = ((var->isStaticArray ?
                                 (Addr)ptrValue :
                                 *((Addr*)(ptrValue))));

      checkAllocAndInitPrintMsg(ptrValue, overrideIsInitialized);

      // If this address hasn't been initialized to anything valid, then
      // we shouldn't try to do anything further with it because it's
      // garbage!!!
      allocatedAndReadable = addressIsInitialized(addressInQuestion, sizeof(void*));

      if (!allocatedAndReadable) {
        DTRACE_PRINTF( "%s\n%d\n",
                       UNINIT,
                       mapInitToModbit(0));

        variableHasBeenObserved = 0;
      }
      else {
        variableHasBeenObserved =
          printDtraceSingleString(var,
                                  ptrValue,
                                  overrideIsInitialized,
                                  disambigOverride);
      }
    }
  }
  // Struct or union type (that is NOT a pointer)
  else if (var->varType->isStructUnionType) {
    DPRINTF("In struct/union branch\n");

    if (isSequence) {
      int limit = upperBound;
      if (kvasir_array_length_limit != -1) {
        limit = min(limit, kvasir_array_length_limit);
      }

      DTRACE_PRINTF( "[ ");
      for (i = 0; i <= limit; i++) {
            DTRACE_PRINTF( "%u ",
                           (unsigned int)(sequenceEltsToOutput[i]));
      }

      DTRACE_PRINTF( "]\n%d\n",
                     mapInitToModbit(1));
    }
    else {
      // Check if first byte of struct is initialized (This is a flaky
      // definition of initialization but we really care about the
      // struct members, not the struct itself since it's just a
      // hashcode)
      if (checkAllocAndInitPrintMsg(ptrValue, overrideIsInitialized)) {
        DTRACE_PRINTF( "%u\n%d\n",
                       ((unsigned int)(ptrValue)),
                       mapInitToModbit(1));
      }
      else {
        variableHasBeenObserved = 0;
      }
    }
  }
  // Base type
  else {
    DaikonDeclaredType decType;
    DPRINTF("In true base type branch\n");

    decType = var->varType->declaredType;

    if (overrideFloatAsDouble && (decType == D_FLOAT)) {
      decType = D_DOUBLE;
    }
    else if (overrideFloatAsDouble && (decType == D_UNSIGNED_FLOAT)) {
      decType = D_UNSIGNED_DOUBLE;
    }

    DPRINTF("Declared type is %d\n", decType);

    if (isSequence) {
      printDtraceBaseValueSequence(var,
                                   decType,
                                   sequenceEltsToOutput,
                                   upperBound,
                                   disambigOverride,
                                   overrideIsInitialized);
    }
    else {
      printDtraceSingleBaseValue(var,
                                 ptrValue,
                                 decType,
                                 overrideIsInitialized,
                                 disambigOverride);
    }

/*     // For flattened static arrays within structs, */
/*     // temporarily set isStaticArray to 0 */
/*     // so that Kvasir doesn't use its own var->upperBounds[0] to */
/*     // determine array dimensions bur rather uses the parameters */
/*     // upperBounds and bytesBetweenElts */
/*     if ((DERIVED_FLATTENED_ARRAY_VAR == varOrigin) && */
/*         var->isStaticArray) { */
/*       var->isStaticArray = 0; */

/*       variableHasBeenObserved = */
/*         printDtraceBaseValue(var, */
/*                              ptrValue, */
/*                              decType, */
/*                              overrideIsInitialized, */
/*                              isSequence, */
/*                              upperBound, */
/*                              bytesBetweenElts, */
/*                              disambigOverride); */

/*       var->isStaticArray = 1; */
/*     } */
/*     else { */
/*       variableHasBeenObserved = */
/*         printDtraceBaseValue(var, */
/*                              ptrValue, */
/*                              decType, */
/*                              overrideIsInitialized, */
/*                              isSequence, */
/*                              upperBound, */
/*                              bytesBetweenElts, */
/*                              disambigOverride); */
/*     } */
  }

  return variableHasBeenObserved;
}

/* Return a file descriptor for a stream with similar semantics to
   what you'd get in a Unix shell by saying ">fname". Prints an error
   and returns -1 if something goes wrong. */
static int openRedirectFile(const char *fname) {
  int new_fd;
  if (fname[0] == '&') {
    new_fd = dup(atoi(fname + 1));
    if (new_fd == -1) {
      VG_(printf)( "Couldn't duplicate FD `%s': %s\n",
	      fname+1, strerror(errno));
      return -1;
    }
  } else {
    new_fd = open(fname, O_WRONLY|O_CREAT|O_LARGEFILE|O_TRUNC, 0666);
    if (new_fd == -1) {
      VG_(printf)( "Couldn't open %s for writing: %s\n",
	      fname, strerror(errno));
      return -1;
    }
  }
  return new_fd;
}

/* Set the buffer for a file handle to a VG_(malloc)ed block, rather than
 * a glibc-malloced one as it would otherwise be. On some systems (e.g.,
 * Red Hat 9 ones) this seems to work around a bug where the two mallocs
 * both think they own an area of memory. It would be better if we could
 * fix the underlying bug, though. */
void fixBuffering(FILE *fp) {
  char *buffer = VG_(malloc)(8192);
  if (setvbuf(fp, buffer, _IOFBF, 8192)) {
     VG_(printf)("setvbuf failed\n");
  }
}

static int gzip_pid = 0;

int openDtraceFile(const char *fname) {
  const char *mode_str;
  char *stdout_redir = kvasir_program_stdout_filename;
  char *stderr_redir = kvasir_program_stderr_filename;

  char *env_val = getenv("DTRACEAPPEND");
  if (env_val || kvasir_dtrace_append) {
    // If we are appending and not printing out separate decls and
    // dtrace files, do NOT print out decls again since we assume that
    // our existing dtrace file already has the decls info in it and
    // we don't want to confuse Daikon (or bloat up the file size) by
    // repeating this information
    if (!actually_output_separate_decls_dtrace) {
      extern char do_not_print_out_decls;
      do_not_print_out_decls = 1;
    }
    mode_str = "a";
  }
  else {
    mode_str = "w";
  }

  // If we're sending trace data to stdout, we definitely don't want the
  // program's output going to the same place.
  if (VG_STREQ(fname, "-") && !stdout_redir) {
    stdout_redir = "/dev/tty";
  }

  if (kvasir_dtrace_gzip || getenv("DTRACEGZIP")) {
    int fds[2]; /* fds[0] for reading (child), fds[1] for writing (parent) */
    pid_t pid;
    int fd;
    int mode;
    char *new_fname = VG_(malloc)(strlen(fname) + 4);
    VG_(strcpy)(new_fname, fname);
    VG_(strcat)(new_fname, ".gz");

    if (pipe(fds) < 0)
      return 0;

    if (!(dtrace_fp = fdopen(fds[1], "w")) || (pid = fork()) < 0) {
      close(fds[0]);
      close(fds[1]);
      return 0;
    }
    fixBuffering(dtrace_fp);

    if (!pid) {
      /* In child */
      char *const argv[] = {"gzip", "-c", 0};
      close(fds[1]);

      /* Redirect stdin from the pipe */
      close(0);
      dup2(fds[0], 0);
      close(fds[0]);

      if (!VG_STREQ(fname, "-")) {
	/* Redirect stdout to the dtrace.gz file */
	mode = O_CREAT | O_LARGEFILE | O_TRUNC |
	  (*mode_str == 'a' ? O_APPEND : O_WRONLY);
	fd = open(new_fname, mode, 0666);
	if (fd == -1) {
	  VG_(printf)( "Couldn't open %s for writing\n", fname);
	}
	close(1);
	dup2(fd, 1);
	close(fd);
      }

      execv("/bin/gzip", argv);
      _exit(127);
    }

    close(fds[0]);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    gzip_pid = pid;
  } else if VG_STREQ(fname, "-") {
    int dtrace_fd = dup(1);
    dtrace_fp = fdopen(dtrace_fd, mode_str);
    if (!dtrace_fp) {
      return 0;
    }
    fixBuffering(dtrace_fp);
  } else {
    dtrace_fp = fopen(fname, mode_str);
    if (!dtrace_fp) {
      return 0;
    }
    fixBuffering(dtrace_fp);
  }

  if (stdout_redir) {
    int new_stdout = openRedirectFile(stdout_redir);
    if (new_stdout == -1)
      return 0;
    close(1);
    dup2(new_stdout, 1);
    if (stderr_redir && VG_STREQ(stdout_redir, stderr_redir)) {
      /* If the same name was supplied for stdout and stderr, do the
	 equivalent of the shell's 2>&1, rather than having them overwrite
	 each other */
      close(2);
      dup2(new_stdout, 2);
      stderr_redir = 0;
    }
    close(new_stdout);
  }

  if (stderr_redir) {
    int new_stderr = openRedirectFile(stderr_redir);
    if (new_stderr == -1)
      return 0;
    close(2);
    dup2(new_stderr, 2);
    close(new_stderr);
  }

  return 1;
}

// Close the stream and finish writing the .dtrace file
// as well as all other open file streams
void finishDtraceFile()
{
  if (dtrace_fp) /* If something goes wrong, we can be called with this null */
    fclose(dtrace_fp);
  if (gzip_pid) {
    int status;
    waitpid(gzip_pid, &status, 0);
    /* Perhaps check return value? */
    gzip_pid = 0;
  }
}

// Print a single hashcode value specified by *ptrValue
// Pre: ptrValue has been initialized to a valid value
char printDtraceSingleHashcode(DaikonVariable* var, void* ptrValue)
{
  DTRACE_PRINTF( "%u\n%d\n", var->isStaticArray ?
                 (Addr)ptrValue :
                 *((Addr*)ptrValue),
                 mapInitToModbit(1));

  // Since we observed all of these bytes as one value,
  // we will merge all of their tags in memory
  if (kvasir_with_dyncomp) {
    DYNCOMP_DPRINTF("dtrace call val_uf_union_tags_in_range(0x%x, %d)\n",
                    ptrValue, sizeof(void*));
    val_uf_union_tags_in_range(ptrValue, sizeof(void*));
  }

  return 1;
}

// Outputs the contents of sequenceEltsToOutput array from [0,
// upperBound] and, as an important side effect, MUTATES the elements
// of sequenceEltsToOutput to now refer to the contents of those
// pointers.
void printDtraceHashcodeSequence(DaikonVariable* var,
                                 void** sequenceEltsToOutput,
                                 unsigned long upperBound)
{
  int i = 0;
  int limit = upperBound;

  void* basePtrValue = sequenceEltsToOutput[0];

  if (kvasir_array_length_limit != -1) {
    limit = min(limit, kvasir_array_length_limit);
  }

  DTRACE_PRINTF( "[ ");
  for (i = 0; i <= limit; i++) {
    void* curAddr = sequenceEltsToOutput[i];
    if (var->isStaticArray) {
      DTRACE_PRINTF("%u ", (Addr)curAddr);
      // No mutation necessary here because we are not dereferencing
      // any additional levels of pointers
    }
    else {
      // Values of 0 specify nulls, so don't bother to check them
      if (curAddr && checkAllocAndInit(curAddr)) {
        Addr ptrContents = *((Addr*)(curAddr));
        DTRACE_PRINTF("%u ", ptrContents);
        // VERY IMPORTANT MUTATION STEP!!!
        sequenceEltsToOutput[i] = ptrContents;
      }
      else {
        // Is 'null' a legal value for Daikon???
        DTRACE_PRINTF("null ");
        // VERY IMPORTANT MUTATION STEP!!!
        sequenceEltsToOutput[i] = 0;
        if (i == 0) {
          basePtrValue = 0;
        }
      }
    }

    // Merge the tags of the 4-bytes of the observed pointer as
    // well as the tags of the base address and the current
    // address because we are observing everything as a sequence
    if (kvasir_with_dyncomp) {
      DYNCOMP_DPRINTF("dtrace call val_uf_union_tags_in_range(0x%x, %d)\n",
                      curAddr, sizeof(void*));
      val_uf_union_tags_in_range(curAddr, sizeof(void*));
      if (basePtrValue) { // Don't merge if basePtrValue is 0
        val_uf_union_tags_at_addr(basePtrValue, curAddr);
      }
    }
  }
  DTRACE_PRINTF( "]\n%d\n",
                 mapInitToModbit(1));
}


// Return 1 if this variable has really been observed,
//        0 if it has not (UNINIT printed out)
char printDtraceSingleString(DaikonVariable* var,
                             void* ptrValue,
                             char overrideIsInitialized,
                             DisambigOverride disambigOverride)
{
  char init = 0;

  DPRINTF("It's a string\n");

  int ptrReadable;
  DPRINTF("A single string\n");
  DPRINTF("ptrValue is %p\n", ptrValue);
  DPRINTF("isStaticArray is %d, isGlobal is %d\n",
          var->isStaticArray, var->isGlobal);
  if (var->isStaticArray) {
    DPRINTF("Dereferenced value is %p", *(char **)ptrValue);
  }

  // Check if the whole string is legit
  ptrReadable = addressIsInitialized((Addr)ptrValue, sizeof(char*));
  if (!ptrReadable && !overrideIsInitialized) {
    init = 0;
    DPRINTF("Pointer is unreadable\n");
  } else {
    init = checkStringReadable(((var->isStaticArray) ?
                                (char*)ptrValue :
                                *(char**)ptrValue));
    DPRINTF("String is%s readable\n", init ? "" : " not");
  }

  if (init)
    {
      if (OVERRIDE_STRING_AS_ONE_CHAR_STRING == disambigOverride) {
        printOneCharAsDtraceString(((var->isStaticArray) ?
                                    *((char*)ptrValue) :
                                    *(*((char**)(ptrValue)))));
      }
      else if (OVERRIDE_STRING_AS_ONE_INT == disambigOverride) {
        char intToPrint = (((var->isStaticArray) ?
                            *((char*)ptrValue) :
                            *(*((char**)(ptrValue)))));

        DTRACE_PRINTF( "%d", intToPrint);
      }
      else if (OVERRIDE_STRING_AS_INT_ARRAY == disambigOverride) {
        printOneDtraceStringAsIntArray(((var->isStaticArray) ?
                                        (char*)ptrValue :
                                        *((char**)(ptrValue))));
      }
      else {
        printOneDtraceString(((var->isStaticArray) ?
                              (char*)ptrValue :
                              *((char**)(ptrValue))));
      }
      DTRACE_PRINTF( "\n%d\n",
                     mapInitToModbit(1));
    }
  else
    {
      DTRACE_PRINTF( "%s\n%d\n",
                     UNINIT,
                     mapInitToModbit(0));

      return 0;
    }

  return 1;
}


// Prints out a sequence of strings from sequenceEltsToOutput Pre:
// Every element of sequenceEltsToOutput is either a char* to a
// null-terminated string or some uninitialized value
void printDtraceStringSequence(DaikonVariable* var,
                               DisambigOverride disambigOverride,
                               void** sequenceEltsToOutput,
                               unsigned long upperBound)
{
  DPRINTF("It's a sequence of strings\n");

  char* currentPtr = 0;
  int ptrReadable = 0;
  int i = 0;

  int limit = upperBound;
  if (kvasir_array_length_limit != -1) {
    limit = min(limit, kvasir_array_length_limit);
  }

  char* basePtrValue = sequenceEltsToOutput[0];

  if (!checkAllocAndInit(basePtrValue)) {
    basePtrValue = 0;
  }

  DTRACE_PRINTF( "[ ");

  for (i = 0; i <= limit; i++) {
    currentPtr = sequenceEltsToOutput[i];

    if (kvasir_with_dyncomp) {
      if (basePtrValue) { // Don't merge stuff for address 0 because that may denote a null
      val_uf_union_tags_at_addr((Addr)basePtrValue, (Addr)currentPtr);
      }
    }

    // Check if the whole string is legit
    ptrReadable = addressIsInitialized((Addr)currentPtr, sizeof(char*));

    if (ptrReadable)
      {
        if (!(var->isStaticArray) || var->isGlobal)
          currentPtr = *(char**)currentPtr;

        if (checkStringReadable(currentPtr))
          {
            if (OVERRIDE_STRING_AS_ONE_CHAR_STRING == disambigOverride) {
              printOneCharAsDtraceString(*currentPtr);
            }
            // Daikon doesn't support nested sequences like this:
            // [ [ 1 2 3 ] [ 4 5 6 ] [ 7 8 9 ] ]
            // so we must resort to only printing out the first entry of each
            // array like [ 1 4 7 ]
            else if ((OVERRIDE_STRING_AS_ONE_INT == disambigOverride) ||
                     (OVERRIDE_STRING_AS_INT_ARRAY == disambigOverride)) {
              char intToPrint = (*currentPtr);
              DTRACE_PRINTF( "%d", intToPrint);
            }
            else {
              printOneDtraceString(currentPtr);
            }
          }
        else
          {
            DTRACE_PRINTF( "null");
          }
      }
    else
      {
        DTRACE_PRINTF( "null");
      }
    DTRACE_PRINTF( " ");
  }

  DTRACE_PRINTF( "]\n%d\n", mapInitToModbit(1));
}


#define PRINT_STATIC_ARRAY(TYPE) \
  DTRACE_PRINTF( TYPE_FORMAT_STRINGS[decType], ((TYPE*)(ptrValue))[i]);
#define PRINT_ARRAY_VAR(TYPE) \
  DTRACE_PRINTF( TYPE_FORMAT_STRINGS[decType], *((TYPE*)(loc)));
#define PRINT_ONE_VAR(TYPE) \
  DTRACE_PRINTF( TYPE_FORMAT_STRINGS[decType], *((TYPE*)(ptrValue)));

#define PRINT_GLOBAL_TEMP_VAR(TYPE) \
  DTRACE_PRINTF( TYPE_FORMAT_STRINGS[decType], *((TYPE*)GLOBAL_TEMP_VAR))


#define BIT_LEVEL_PRINT_STATIC_ARRAY(TYPE) \
  apply_mask_to_bytes((char*)(ptrValue + (i*TYPE_BYTE_SIZES[decType])), TYPE_BYTE_SIZES[decType]); \
  PRINT_GLOBAL_TEMP_VAR(TYPE);

#define BIT_LEVEL_PRINT_ARRAY_VAR(TYPE) \
  apply_mask_to_bytes((char*)loc, TYPE_BYTE_SIZES[decType]);  \
  PRINT_GLOBAL_TEMP_VAR(TYPE);

#define BIT_LEVEL_PRINT_ONE_VAR(TYPE) \
  apply_mask_to_bytes((char*)ptrValue, TYPE_BYTE_SIZES[decType]);   \
  PRINT_GLOBAL_TEMP_VAR(TYPE);


#define CLEAR_GLOBAL_MASK_STUFF() \
  VG_(memset)(GLOBAL_TEMP_VAR, 0, 8); \
  VG_(memset)(GLOBAL_MASK, 0, 8)

// Applies 'len' bytes of the bit-mask 'GLOBAL_MASK' to data stored in
// 'location' and stores the result in GLOBAL_TEMP_VAR;
// Pre: 0 < len <= 8 because GLOBAL_TEMP_VAR is only 8 bytes big,
//      The first 'len' bytes of GLOBAL_MASK initialized to the proper mask.
// DO NOT modify *location because that will alter
// the behavior of the real program.
void apply_mask_to_bytes(char* location, int len) {
  int i;
  for (i = 0; i < len; i++) {
    GLOBAL_TEMP_VAR[i] = (location[i] & GLOBAL_MASK[i]);
    //    GLOBAL_TEMP_VAR[i] = (location[i]); // & GLOBAL_MASK[i]);
  }
}


#define TYPES_SWITCH(OPERATION) \
switch (decType) \
{ \
 case D_BOOL: \
 case D_UNSIGNED_CHAR: \
   OPERATION(unsigned char) \
   break; \
 case D_CHAR: \
   OPERATION(char) \
   break; \
 case D_UNSIGNED_SHORT: \
   OPERATION(unsigned short) \
   break; \
 case D_SHORT: \
   OPERATION(short) \
   break; \
 case D_UNSIGNED_INT: \
   OPERATION(unsigned int) \
   break; \
 case D_INT: \
 case D_ENUMERATION: \
   OPERATION(int) \
   break; \
 case D_UNSIGNED_LONG_LONG_INT: \
   OPERATION(unsigned long long int) \
   break; \
 case D_LONG_LONG_INT: \
   OPERATION(long long int) \
   break; \
 case D_UNSIGNED_FLOAT: \
 case D_FLOAT: \
   OPERATION(float) \
   break; \
 case D_UNSIGNED_DOUBLE: \
 case D_DOUBLE: \
   OPERATION(double) \
   break; \
 default: \
   DTRACE_PRINTF( "TYPES_SWITCH()\n - unknown type"); \
   DABORT("TYPES_SWITCH()\n - unknown type"); \
   break; \
}


/* For the benefit of the DPRINTF below */
extern const char* DaikonDeclaredTypeString[20];

// Return 1 if this variable has really been observed,
//        0 if it has not (UNINIT printed out)
char printDtraceSingleBaseValue(DaikonVariable* var,
                                char* ptrValue,
                                // not necessarily the same as var's
                                // declared type
                                DaikonDeclaredType decType,
                                char overrideIsInitialized,
                                DisambigOverride disambigOverride)
{
  int init = 0;

  DPRINTF(DaikonDeclaredTypeString[decType]);
  DPRINTF(" branch - printDtraceBaseValue()\n");

  // This check is to make sure that we don't segfault
  if (!overrideIsInitialized && !(addressIsAllocated((Addr)ptrValue, TYPE_BYTE_SIZES[decType])))
    {
      DABORT("var %s is NOT allocated!\n", var->name);
      // If we want to mask the bugs, just print out NONSENSICAL and continue onwards
      if (!kvasir_asserts_aborts_on)
	{
	  DTRACE_PRINTF( "%s\n%d\n",
		  NONSENSICAL,
		  mapInitToModbit(init));
	  return 0;
	}
    }

  CLEAR_GLOBAL_MASK_STUFF();

  if (overrideIsInitialized) {
    VG_(memset)(GLOBAL_MASK, 0xFF, 8);
    init = 1;
  }
  else {
    if (kvasir_use_bit_level_precision) {
      init = are_some_bytes_init((Addr)ptrValue, TYPE_BYTE_SIZES[decType]);
      // GLOBAL_MASK initialized in are_some_bytes_init()!
    }
    else {
      init = addressIsInitialized((Addr)ptrValue, TYPE_BYTE_SIZES[decType]);
    }
  }

  // Don't support printing of these types:
  if ((decType == D_FUNCTION) || (decType == D_VOID)) {
    init = 0;
  }

  if (init) {
    // Special case for .disambig:
    if (OVERRIDE_CHAR_AS_STRING == disambigOverride) {
      printOneCharAsDtraceString(*((char*)ptrValue));
      DTRACE_PRINTF( "\n%d\n", mapInitToModbit(1));
    }
    else {
      DPRINTF("In single-value branch\n");
      if (kvasir_use_bit_level_precision) {
        TYPES_SWITCH(BIT_LEVEL_PRINT_ONE_VAR)}
      else {TYPES_SWITCH(PRINT_ONE_VAR)}

      if (kvasir_with_dyncomp) {
        DYNCOMP_DPRINTF("dtrace call val_uf_union_tags_in_range(0x%x, %d)\n",
                        (Addr)ptrValue, TYPE_BYTE_SIZES[decType]);
        val_uf_union_tags_in_range((Addr)ptrValue, TYPE_BYTE_SIZES[decType]);
      }

      DTRACE_PRINTF( "\n%d\n", mapInitToModbit(1));
    }

    return 1;
  }
  // Print out "uninit" and modbit=2 for uninitialized values
  else {
    DTRACE_PRINTF( "%s\n%d\n",
                   UNINIT,
                   mapInitToModbit(init));
      return 0;
  }
}

void printDtraceBaseValueSequence(DaikonVariable* var,
                                  // not necessarily the same as var's
                                  // declared type
                                  DaikonDeclaredType decType,
                                  void** sequenceEltsToOutput,
                                  unsigned long upperBound,
                                  DisambigOverride disambigOverride,
                                  char overrideIsInitialized)
{
  int i = 0;

  void* basePtrValue = sequenceEltsToOutput[0];
  if (!checkAllocAndInit(basePtrValue)) {
    basePtrValue = 0;
  }

  DPRINTF(DaikonDeclaredTypeString[decType]);
  DPRINTF(" branch - printDtraceBaseValue()\n");

  // Don't support printing of these types:
  if ((decType == D_FUNCTION) || (decType == D_VOID)) {
    DTRACE_PRINTF( "%s\n%d\n",
                   UNINIT,
                   mapInitToModbit(0));
    return;
  }

/*       if VAR_IS_STATIC_ARRAY(var) */
/* 	{ */
/* 	  DPRINTF("In static array branch\n"); */

/* 	  if (OVERRIDE_ARRAY_AS_POINTER == disambigOverride) { */
/* 	    DPRINTF(" STATIC ARRAY: %s\n", var->name); */
/*             if (kvasir_use_bit_level_precision) { */
/*               CLEAR_GLOBAL_MASK_STUFF(); */
/*               are_some_bytes_init((Addr)ptrValue, TYPE_BYTE_SIZES[decType]); */
/*               TYPES_SWITCH(BIT_LEVEL_PRINT_STATIC_ARRAY)} */
/* 	    else {TYPES_SWITCH(PRINT_STATIC_ARRAY)} */
/* 	      DTRACE_PRINTF( "\n%d\n", */
/* 		      mapInitToModbit(1)); */

/*               if (kvasir_with_dyncomp) { */
/*                 DYNCOMP_DPRINTF("dtrace call val_uf_union_tags_in_range(0x%x, %d)\n", */
/*                                 (Addr)ptrValue, TYPE_BYTE_SIZES[decType]); */
/*                 val_uf_union_tags_in_range((Addr)ptrValue, TYPE_BYTE_SIZES[decType]); */
/*               } */
/* 	  } */
/* 	  else { */
/*             int limit = var->upperBounds[0]; */
/*             if (kvasir_array_length_limit != -1) */
/*               limit = min(limit, kvasir_array_length_limit); */
/* 	    DTRACE_PRINTF( "[ "); */
/* 	    for (i = 0; i <= limit; i++) */
/* 	      { */
/*                 Addr curAddr = (Addr)(ptrValue + (i*TYPE_BYTE_SIZES[decType])); */
/*                 if (kvasir_use_bit_level_precision) { */
/*                   CLEAR_GLOBAL_MASK_STUFF(); */
/*                   are_some_bytes_init(curAddr, */
/*                                       TYPE_BYTE_SIZES[decType]); */
/*                   TYPES_SWITCH(BIT_LEVEL_PRINT_STATIC_ARRAY)} */
/*                 else {TYPES_SWITCH(PRINT_STATIC_ARRAY)} */

/*                 if (kvasir_with_dyncomp) { */
/*                   DYNCOMP_DPRINTF("dtrace call val_uf_union_tags_in_range(0x%x, %d)\n", */
/*                                   curAddr, TYPE_BYTE_SIZES[decType]); */
/*                   val_uf_union_tags_in_range(curAddr, TYPE_BYTE_SIZES[decType]); */
/*                   val_uf_union_tags_at_addr((Addr)ptrValue, curAddr); */
/*                 } */

/*                 DTRACE_PRINTF( " "); */
/* 	      } */
/* 	    DTRACE_PRINTF( "]\n%d\n", */
/* 		    mapInitToModbit(1)); */
/* 	  } */
/* 	} */

  int limit = upperBound;
  if (kvasir_array_length_limit != -1) {
    limit = min(limit, kvasir_array_length_limit);
  }

  DTRACE_PRINTF( "[ ");

  for (i = 0; i <= limit; i++) {
    Addr loc = (Addr)sequenceEltsToOutput[i];

    // TODO: Changed this check from IsReadable to IsAllocated
    // because we don't want to deref an invalid address:
    int okay = addressIsAllocated(loc, TYPE_BYTE_SIZES[decType]);
    char eltInitialized = 0;

    // This is where we mask the failure that occurs when
    // our upperBound/bytesBetweenElts is messed-up
    if (!okay) {
      if (!kvasir_asserts_aborts_on) {
        // Only mask the failure if !kvasir_asserts_aborts_on
        break;
      }
      else {
        DPRINTF("Bad datum at location %d (out of 0 .. %u) "
                "in array %s at %p\n", i, upperBound, var->name,
                (void *)loc);
        fflush(dtrace_fp);
        DASSERT(okay);
      }
    }

    if (overrideIsInitialized) {
      eltInitialized = 1;
      VG_(memset)(GLOBAL_MASK, 0xFF, 8);
    }
    else {
      if (kvasir_use_bit_level_precision) {
        CLEAR_GLOBAL_MASK_STUFF();
        eltInitialized = are_some_bytes_init((Addr)loc,
                                             TYPE_BYTE_SIZES[decType]);
      }
      else {
        eltInitialized = addressIsInitialized(loc, TYPE_BYTE_SIZES[decType]);
      }
    }

    // If a particular element of an array is not initialized,
    // simply print out 0 so that we never print out uninitialized
    // values but the array indices of initialized values are preserved
    if (!eltInitialized) {
      DTRACE_PRINTF( "0 ");
    }
    else if (OVERRIDE_CHAR_AS_STRING == disambigOverride) {
      printOneCharAsDtraceString(*((char*)(loc)));
      DTRACE_PRINTF( " ");

      if (kvasir_with_dyncomp) {
        DYNCOMP_DPRINTF("dtrace call val_uf_union_tags_in_range(0x%x, %d)\n",
                        loc, TYPE_BYTE_SIZES[decType]);
        val_uf_union_tags_in_range(loc, TYPE_BYTE_SIZES[decType]);
        if (basePtrValue) {
          val_uf_union_tags_at_addr((Addr)basePtrValue, loc);
        }
      }
    }
    else {
      if (kvasir_use_bit_level_precision) {
        TYPES_SWITCH(BIT_LEVEL_PRINT_ARRAY_VAR)}
      else {TYPES_SWITCH(PRINT_ARRAY_VAR)}

      if (kvasir_with_dyncomp) {
        DYNCOMP_DPRINTF("dtrace call val_uf_union_tags_in_range(0x%x, %d)\n",
                          loc, TYPE_BYTE_SIZES[decType]);
        val_uf_union_tags_in_range(loc, TYPE_BYTE_SIZES[decType]);
        if (basePtrValue) {
          val_uf_union_tags_at_addr((Addr)basePtrValue, loc);
          }
      }

      DTRACE_PRINTF(" ");
    }
  }
  DTRACE_PRINTF( "]\n%d\n",
                 mapInitToModbit(1));
}
