/*
   This file is part of Kvasir, a C/C++ front end for the Daikon
   dynamic invariant detector built upon the Fjalar framework

   Copyright (C) 2004-2006 Philip Guo (pgbovine@alum.mit.edu),
   MIT CSAIL Program Analysis Group

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

#include "../fjalar_include.h"

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


// The indices to this array must match the DeclaredType enum
// declared in generate_fjalar_entries.h:
static const char* TYPE_FORMAT_STRINGS[] = {
  "%d - ERROR - D_NO_TYPE",        // D_NO_TYPE, // Create padding

  "%u",                            // D_UNSIGNED_CHAR,
  "%d",                            // D_CHAR
  "%hu",                           // D_UNSIGNED_SHORT,
  "%hd",                           // D_SHORT,
  "%u",                            // D_UNSIGNED_INT,
  "%d",                            // D_INT,
  "%llu",                          // D_UNSIGNED_LONG_LONG_INT,
  "%lld",                          // D_LONG_LONG_INT,

  "%.9g",                          // D_FLOAT,
  "%.17g",                         // D_DOUBLE,
  "%.17g",                         // D_LONG_DOUBLE,

  "%d",                            // D_ENUMERATION,
  "%d - ERROR - D_STRUCT",         // D_STRUCT,  // currently unused
  "%d - ERROR - D_UNION",          // D_UNION,   // currently unused

  "%d - ERROR - D_FUNCTION",       // D_FUNCTION // currently unused
  "%d - ERROR - D_VOID",           // D_VOID     // currently unused
  "%d - ERROR - D_CHAR_AS_STRING", // D_CHAR_AS_STRING
  "%d" ,                           // D_BOOL
};

// The indices to this array must match the DeclaredType enum
// declared in generate_fjalar_entries.h:
extern const int DecTypeByteSizes[];

extern void printDaikonFunctionName(FunctionEntry* f, FILE* fp);

// If there are function names (e.g., C++ demangled names) that are
// illegal for Daikon, we can patch them up here before writing them
// to the .dtrace file:
void printDtraceFunctionHeader(FunctionEntry* funcPtr, char isEnter)
{
  DPRINTF("Printing dtrace header for %s\n", funcPtr->fjalar_name);
  DPRINTF("dtrace_fp is %p\n", dtrace_fp);
  tl_assert(dtrace_fp);

  fputs("\n", dtrace_fp);
  printDaikonFunctionName(funcPtr, dtrace_fp);

  if (isEnter) {
    fputs(ENTER_PPT, dtrace_fp);
    fputs("\n", dtrace_fp);
  }
  else {
    fputs(EXIT_PPT, dtrace_fp);
    fputs("\n", dtrace_fp);
  }

  DPRINTF("Done printing header for %s\n", funcPtr->fjalar_name);
}

// Maps init value to modbit
// init = 1 ---> modbit = 1
// init = 0 ---> modbit = 2
static char mapInitToModbit(char init)
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

// Prints a string to dtrace_fp, keeping in mind to quote
// special characters so that the lines don't get screwed up
static void printOneDtraceString(char* str)
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
        VG_(printf)("  Error!  Ran into unreadable character!\n");
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
static void printOneCharAsDtraceString(char c)
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

static void printOneDtraceStringAsIntArray(char* str) {
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
        VG_(printf)("  Error!  Ran into unreadable character!\n");
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


#define PRINT_STATIC_ARRAY(TYPE) \
  DTRACE_PRINTF( TYPE_FORMAT_STRINGS[decType], ((TYPE*)(ptrValue))[i]);
#define PRINT_ARRAY_VAR(TYPE) \
  DTRACE_PRINTF( TYPE_FORMAT_STRINGS[decType], *((TYPE*)(loc)));
#define PRINT_ONE_VAR(TYPE) \
  DTRACE_PRINTF( TYPE_FORMAT_STRINGS[decType], *((TYPE*)(ptrValue)));


// Macro for dispatching on different print statements depending on
// the declared type of the variable (decType) - creates a switch
// statement parameterized by the OPERATION macro:
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
   DTRACE_PRINTF( "TYPES_SWITCH() - unknown type"); \
   tl_assert(0 && "TYPES_SWITCH() - unknown type"); \
   break; \
}


#define DTRACE_PRINT_ONE_VAR(TYPE) \
  DTRACE_PRINTF( TYPE_FORMAT_STRINGS[decType], *((TYPE*)(pValue)));

#define DTRACE_PRINT_ONE_VAR_WITHIN_SEQUENCE(TYPE) \
  DTRACE_PRINTF( TYPE_FORMAT_STRINGS[decType], *((TYPE*)(pCurValue)));

static char printDtraceSingleBaseValue(void* pValue,
                                       DeclaredType decType,
                                       char overrideIsInit,
                                       DisambigOverride disambigOverride);

static void printDtraceBaseValueSequence(DeclaredType decType,
                                         void** pValueArray,
                                         UInt numElts,
                                         DisambigOverride disambigOverride,
                                         void** pFirstInitElt);

static void printDtraceSingleString(char* actualString,
                                    DisambigOverride disambigOverride);


static void printDtraceStringSequence(VariableEntry* var,
                                      void** pValueArray,
                                      UInt numElts,
                                      DisambigOverride disambigOverride,
                                      void** pFirstInitElt);


// Prints a .dtrace entry for a single variable value denoted by
// pValue.  Returns 1 if variable successfully observed and printed,
// and 0 otherwise.
char printDtraceSingleVar(VariableEntry* var,
                          void* pValue,
                          VariableOrigin varOrigin,
                          char isHashcode,
                          char overrideIsInit,
                          DisambigOverride disambigOverride) {
  char allocated = 0;
  char initialized = 0;

  tl_assert(var);

  //  VG_(printf)("  printDtraceSingleVar(): %p\n", pValue);

  // a pValue of 0 means nonsensical because there is no content to
  // dereference:
  if (!pValue) {
    DTRACE_PRINTF("%s\n%d\n",
                  NONSENSICAL,
                  mapInitToModbit(0));
    return 0;
  }

  // At minimum, check whether the first byte is allocated and/or
  // initialized
  allocated = (overrideIsInit ? 1 :
               addressIsAllocated((Addr)pValue, sizeof(char)));

  if (!allocated) {
    DTRACE_PRINTF("%s\n%d\n",
                  NONSENSICAL,
                  mapInitToModbit(0));
    return 0;
  }

  initialized = (overrideIsInit ? 1 :
                 addressIsInitialized((Addr)pValue, sizeof(char)));

  if (!initialized) {
    DTRACE_PRINTF("%s\n%d\n",
                  UNINIT,
                  mapInitToModbit(0));
    return 0;
  }

  // From this point onwards we know that pValue is safe to
  // dereference because it has been both allocated and initialized

  // Pointer (put this check first before the var->isString check so
  // that it will work even for pointers to strings):
  if (isHashcode) {
    // Be careful of what to print depending on whether the
    // variable is a static array:
    // TODO: What about a pointer to a static array?
    //       var->isStaticArray says that the base variable is a
    //       static array after all dereferences are done.
    DTRACE_PRINTF("%u\n%d\n",
                  var->isStaticArray ? (UInt)pValue : (UInt)(*((Addr*)pValue)),
                  mapInitToModbit(1));

    // Since we observed all of these bytes as one value, we will
    // merge all of their tags in memory
    if (kvasir_with_dyncomp) {
      DYNCOMP_DPRINTF("dtrace call val_uf_union_tags_in_range(0x%x, %d)\n",
                      pValue, sizeof(void*));
      val_uf_union_tags_in_range((Addr)pValue, sizeof(void*));
    }
  }
  // String (not pointer to string)
  else if (var->isString) {
    char stringReadable = 0;

    // Depends on whether the variable is a static array or not:
    char* actualString = (var->isStaticArray ?
                          pValue :
                          *((char**)pValue));

    // If this address hasn't been initialized to anything valid,
    // then we shouldn't try to do anything further with it because
    // it's garbage!!!
    stringReadable = checkStringReadable(actualString);

    if (stringReadable) {
      printDtraceSingleString(actualString,
                              disambigOverride);
    }
    else {
      DTRACE_PRINTF("%s\n%d\n",
                    UNINIT,
                    mapInitToModbit(0));
      return 0;
    }
  }
  // Base (non-hashcode) struct or union type
  // Simply print out its hashcode location
  else if (var->varType->isStructUnionType) {
    DTRACE_PRINTF("%u\n%d\n",
                  ((unsigned int)pValue),
                  mapInitToModbit(1));
  }
  // Base type
  else {
    DeclaredType decType = var->varType->decType;

    // override float as double when printing
    // out function return variables because
    // return variables stored in %EAX are always doubles
    char overrideFloatAsDouble = (varOrigin == FUNCTION_RETURN_VAR);

    if (overrideFloatAsDouble && (decType == D_FLOAT)) {
      decType = D_DOUBLE;
    }
    else if (overrideFloatAsDouble && (decType == D_UNSIGNED_FLOAT)) {
      decType = D_UNSIGNED_DOUBLE;
    }

    return printDtraceSingleBaseValue(pValue,
                                      decType,
                                      overrideIsInit,
                                      disambigOverride);
  }

  // Default return value:
  return 1;
}


// Prints a .dtrace entry for a sequence of variable values denoted by
// pValueArray (size numElts).  Returns 1 if variable successfully
// observed and printed, and 0 otherwise.
//
// Upon exit, if pFirstInitElt, then *pFirstInitElt contains the
// pointer to the first initialized element in the sequence, or 0 if
// there are no initialized elements in the sequence.  This is useful
// for DynComp to determine which memory location to use as the
// canonical one for the entire sequence in terms of getting tags.
char printDtraceSequence(VariableEntry* var,
                         void** pValueArray,
                         UInt numElts,
                         VariableOrigin varOrigin,
                         char isHashcode,
                         DisambigOverride disambigOverride,
                         void** pFirstInitElt) {
  UInt i;
  char someEltNonZero = 0;
  char someEltInit = 0;

  char firstInitEltFound = 0;
  void* firstInitElt = 0;

  if (pFirstInitElt) {
    *pFirstInitElt = 0;
  }

  tl_assert(var);

  // a pValueArray of 0 or numElts of 0 means nonsensical because
  // there is no content to dereference:
  if (!pValueArray || !numElts) {
    DTRACE_PRINTF("%s\n%d\n",
                  NONSENSICAL,
                  mapInitToModbit(0));
    return 0;
  }

  // If all elements of pValueArray are 0, then this also means
  // nonsensical because there is no content to dereference:
  for (i = 0; i < numElts; i++) {
    if (pValueArray[i]) {
      someEltNonZero = 1;
      break;
    }
  }
  if (!someEltNonZero) {
    DTRACE_PRINTF("%s\n%d\n",
                  NONSENSICAL,
                  mapInitToModbit(0));
    return 0;
  }


  // If all elements in pValueArray are uninit, then print out UNINIT
  // and return 0. (be conservative and only check the first byte so that
  // we don't mistakenly mark an array of shorts as uninitialized)
  for (i = 0; i < numElts; i++) {
    void* pCurValue = pValueArray[i];
    char eltInit = addressIsInitialized((Addr)pCurValue, sizeof(char));
    if (eltInit) {
      someEltInit = 1;
      break;
    }
  }

  if (!someEltInit) {
    DTRACE_PRINTF("%s\n%d\n",
                  UNINIT,
                  mapInitToModbit(0));
    return 0;
  }


  // Pointer (put this check first before the var->isString check so
  // that it will work even for pointers to strings):
  if (isHashcode) {
      int ind;
      int limit = numElts;
      if (fjalar_array_length_limit != -1) {
        limit = min(limit, fjalar_array_length_limit);
      }

      DTRACE_PRINTF( "[ ");

      for (ind = 0; ind < limit; ind++) {
        void* pCurValue = pValueArray[ind];

        char eltInit = addressIsInitialized((Addr)pCurValue, sizeof(void*));

        if (eltInit) {
          if (!firstInitEltFound) {
            firstInitElt = pCurValue;
            firstInitEltFound = 1;
          }

          DTRACE_PRINTF("%u ", var->isStaticArray ?
                        (UInt)pCurValue :
                        (UInt)(*((Addr*)pCurValue)));

          // Merge the tags of the 4-bytes of the observed pointer as
          // well as the tags of the first initialized address and the
          // current address because we are observing everything as a
          // sequence
          // TODO: This may cause unnecessarily large comparability
          // sets - watch out!
          if (kvasir_with_dyncomp && firstInitElt) {
            val_uf_union_tags_in_range((Addr)pCurValue, sizeof(void*));
            val_uf_union_tags_at_addr((Addr)firstInitElt, (Addr)pCurValue);
          }
        }
        else {
          // Daikon currently only supports 'nonsensical' values
          // inside of sequences, not 'uninit' value.
          if (!kvasir_repair_format) {
            DTRACE_PRINTF(NONSENSICAL);
            DTRACE_PRINTF(" ");
          }
        }
      }

      DTRACE_PRINTF( "]\n%d\n",
                     mapInitToModbit(1));
  }
  // String (not pointer to string)
  else if (var->isString) {
    printDtraceStringSequence(var,
                              pValueArray,
                              numElts,
                              disambigOverride,
                              &firstInitElt);
  }
  // Base (non-hashcode) struct or union type
  // Simply print out its hashcode location
  else if (var->varType->isStructUnionType) {
    int ind;
    int limit = numElts;
    if (fjalar_array_length_limit != -1) {
      limit = min(limit, fjalar_array_length_limit);
    }

    DTRACE_PRINTF( "[ ");

    for (ind = 0; ind < limit; ind++) {
      void* pCurValue = pValueArray[ind];
      DTRACE_PRINTF("%u ", (UInt)pCurValue);
    }

    DTRACE_PRINTF( "]\n%d\n",
                   mapInitToModbit(1));
  }
  // Base type
  else {
    DeclaredType decType = var->varType->decType;

    // override float as double when printing
    // out function return variables because
    // return variables stored in %EAX are always doubles
    char overrideFloatAsDouble = (varOrigin == FUNCTION_RETURN_VAR);

    if (overrideFloatAsDouble && (decType == D_FLOAT)) {
      decType = D_DOUBLE;
    }
    else if (overrideFloatAsDouble && (decType == D_UNSIGNED_FLOAT)) {
      decType = D_UNSIGNED_DOUBLE;
    }

    printDtraceBaseValueSequence(decType,
                                 pValueArray,
                                 numElts,
                                 disambigOverride,
                                 &firstInitElt);
  }

  if (pFirstInitElt) {
    *pFirstInitElt = firstInitElt;
  }

  // Default return value:
  return 1;
}


// Print a single numerical value to .dtrace pointed-to by pValue
static
char printDtraceSingleBaseValue(void* pValue,
                                DeclaredType decType,
                                char overrideIsInit,
                                DisambigOverride disambigOverride) {
  int init = 0;

  // This check is to make sure that we don't segfault
  if (!overrideIsInit &&
      !(addressIsAllocated((Addr)pValue, DecTypeByteSizes[decType]))) {
    DTRACE_PRINTF("%s\n%d\n",
                  NONSENSICAL,
                  mapInitToModbit(0));
    return 0;
  }

  if (overrideIsInit) {
    init = 1;
  }
  else {
    init = addressIsInitialized((Addr)pValue, DecTypeByteSizes[decType]);
  }

  // Don't support printing of these types:
  if ((decType == D_FUNCTION) || (decType == D_VOID)) {
    init = 0;
  }

  if (init) {
    // Special case for .disambig:
    if (OVERRIDE_CHAR_AS_STRING == disambigOverride) {
      printOneCharAsDtraceString(*((char*)pValue));
      DTRACE_PRINTF( "\n%d\n", mapInitToModbit(1));
    }
    else {
      TYPES_SWITCH(DTRACE_PRINT_ONE_VAR)

      if (kvasir_with_dyncomp) {
        DYNCOMP_DPRINTF("dtrace call val_uf_union_tags_in_range(0x%x, %d)\n",
                        (Addr)pValue, DecTypeByteSizes[decType]);
        val_uf_union_tags_in_range((Addr)pValue, DecTypeByteSizes[decType]);
      }

      DTRACE_PRINTF( "\n%d\n", mapInitToModbit(1));
    }
    return 1;
  }
  // Print out "uninit" and modbit=2 for uninitialized values
  else {
    DTRACE_PRINTF("%s\n%d\n",
                  UNINIT,
                  mapInitToModbit(0));
    return 0;
  }
}

// Print a sequence of numerical values of declared type decType
// pointed-to by elements of pValueArray.  Also print the valid modbit
// of 1.
// Pre: At least some values pointed-to by elements in pValueArray are
// initialized so we will always print at least some values in the
// sequence.  We print uninitialized values as 'nonsensical' in the
// sequence because that is all Daikon can support at the moment.
// (The one exception is the rare D_FUNCTION or D_VOID types, which
//  we just punt)
//
// Upon exit, if pFirstInitElt, then *pFirstInitElt contains the
// pointer to the first initialized element in the sequence, or 0 if
// there are no initialized elements in the sequence.  This is useful
// for DynComp to determine which memory location to use as the
// canonical one for the entire sequence in terms of getting tags.
//
// Sample output:
// [ 1 2 3 4 5 ]
// 1
static
void printDtraceBaseValueSequence(DeclaredType decType,
                                  void** pValueArray,
                                  UInt numElts,
                                  DisambigOverride disambigOverride,
                                  void** pFirstInitElt) {
  int i = 0;
  int limit = numElts;
  char firstInitEltFound = 0;
  void* firstInitElt = 0;

  if (fjalar_array_length_limit != -1) {
    limit = min(limit, fjalar_array_length_limit);
  }

  // TODO: Add support for bit-level precision here

  // Don't support printing of these types:
  if ((decType == D_FUNCTION) || (decType == D_VOID)) {
    // Just punt
    DTRACE_PRINTF("%s\n%d\n",
                  NONSENSICAL,
                  mapInitToModbit(0));
    return;
  }

  DTRACE_PRINTF( "[ ");

  for (i = 0; i < limit; i++) {
    void* pCurValue = pValueArray[i];

    // Check if it's initialized based on the size of declared type (I
    // hope that everything that's initialized is also allocated):
    char eltInit = addressIsInitialized((Addr)pCurValue, DecTypeByteSizes[decType]);

    if (eltInit) {
      if (!firstInitEltFound) {
        firstInitElt = pCurValue;
        firstInitEltFound = 1;
      }

      // Special case for .disambig:
      if (OVERRIDE_CHAR_AS_STRING == disambigOverride) {
        printOneCharAsDtraceString(*((char*)pCurValue));
      }
      else {
        TYPES_SWITCH(DTRACE_PRINT_ONE_VAR_WITHIN_SEQUENCE)

        // Merge the tags of all bytes read for this element:
        if (kvasir_with_dyncomp) {
          val_uf_union_tags_in_range((Addr)pCurValue, DecTypeByteSizes[decType]);
        }
      }

      // Merge the tags of this element and the first initialized
      // element:
      if (kvasir_with_dyncomp && firstInitElt) {
        val_uf_union_tags_at_addr((Addr)firstInitElt, (Addr)pCurValue);
      }

      DTRACE_PRINTF(" ");
    }
    else {
      // Daikon currently only supports 'nonsensical' values
      // inside of sequences, not 'uninit' value.

      if (!kvasir_repair_format) {
        DTRACE_PRINTF(NONSENSICAL);
        DTRACE_PRINTF(" ");
      }
    }
  }

  DTRACE_PRINTF("]\n%d\n",
                mapInitToModbit(1));

  // Set return value via pointer:
  if (pFirstInitElt) {
    *pFirstInitElt = firstInitElt;
  }
}

// Pre: actualString is an initialized null-terminated C string
static
void printDtraceSingleString(char* actualString,
                             DisambigOverride disambigOverride) {
  if (OVERRIDE_STRING_AS_ONE_CHAR_STRING == disambigOverride) {
    printOneCharAsDtraceString(actualString[0]);
  }
  else if (OVERRIDE_STRING_AS_ONE_INT == disambigOverride) {
    char intToPrint = actualString[0];
    DTRACE_PRINTF( "%d", intToPrint);
  }
  else if (OVERRIDE_STRING_AS_INT_ARRAY == disambigOverride) {
    printOneDtraceStringAsIntArray(actualString);
  }
  else {
    printOneDtraceString(actualString);
  }

  DTRACE_PRINTF("\n%d\n",
                mapInitToModbit(1));
}


// Print a sequence of strings pointed-to by elements of pValueArray.
// Also print the valid modbit of 1.
//
// Pre: At least some values pointed-to by elements in pValueArray are
// initialized so we will always print at least some values in the
// sequence.  We print uninitialized values as 'nonsensical' in the
// sequence because that is all Daikon can support at the moment.
//
// Upon exit, if pFirstInitElt, then *pFirstInitElt contains the
// pointer to the first initialized element in the sequence, or 0 if
// there are no initialized elements in the sequence.  This is useful
// for DynComp to determine which memory location to use as the
// canonical one for the entire sequence in terms of getting tags.
//
// Sample output:
// [ "hello" "world" "foo" ]
// 1
static
void printDtraceStringSequence(VariableEntry* var,
                               void** pValueArray,
                               UInt numElts,
                               DisambigOverride disambigOverride,
                               void** pFirstInitElt) {
  int i = 0;
  int limit = numElts;
  char firstInitEltFound = 0;
  void* firstInitElt = 0;

  if (fjalar_array_length_limit != -1) {
    limit = min(limit, fjalar_array_length_limit);
  }

  DTRACE_PRINTF( "[ ");

  for (i = 0; i < limit; i++) {
    char* pCurValue = (char*)pValueArray[i];
    char eltInit = addressIsInitialized((Addr)pCurValue, sizeof(char*));

    if (eltInit) {
      if (!firstInitEltFound) {
        firstInitElt = pCurValue;
        firstInitEltFound = 1;
      }

      // Merge the tags of this element and the first initialized
      // element:
      if (kvasir_with_dyncomp && firstInitElt) {
        val_uf_union_tags_at_addr((Addr)firstInitElt, (Addr)pCurValue);
      }

      if (!(var->isStaticArray) || var->isGlobal) {
        pCurValue = *(char**)pCurValue;
      }

      if (checkStringReadable(pCurValue)) {
        if (OVERRIDE_STRING_AS_ONE_CHAR_STRING == disambigOverride) {
          printOneCharAsDtraceString(pCurValue[0]);
        }
        // Daikon doesn't support nested sequences like this:
        // [ [ 1 2 3 ] [ 4 5 6 ] [ 7 8 9 ] ]
        // so we must resort to only printing out the first entry of each
        // array like [ 1 4 7 ]
        else if ((OVERRIDE_STRING_AS_ONE_INT == disambigOverride) ||
                 (OVERRIDE_STRING_AS_INT_ARRAY == disambigOverride)) {
          char intToPrint = pCurValue[0];
          DTRACE_PRINTF( "%d", intToPrint);
        }
        else {
          printOneDtraceString(pCurValue);
        }

        DTRACE_PRINTF(" ");
      }
      else {
        // Daikon currently only supports 'nonsensical' values
        // inside of sequences, not 'uninit' value.
        if (!kvasir_repair_format) {
          DTRACE_PRINTF(NONSENSICAL);
          DTRACE_PRINTF(" ");
        }
      }
    }
    else {
      if (!kvasir_repair_format) {
        DTRACE_PRINTF(NONSENSICAL);
        DTRACE_PRINTF(" ");
      }
    }
  }

  DTRACE_PRINTF("]\n%d\n",
                mapInitToModbit(1));

  // Set return value via pointer:
  if (pFirstInitElt) {
    *pFirstInitElt = firstInitElt;
  }
}

// This is where all of the action happens!
// Prints out a .dtrace entry for a variable.
// This consists of 3 lines:
// foo.bar.a   <- full variable name
// 42          <- value of the variable
// 1           <- 'modbit' - 1 for valid, 2 for invalid

// Returns DEREF_MORE_POINTERS if the variable has been successfully
// observed, DO_NOT_DEREF_MORE_POINTERS otherwise, in which case all
// subsequent traversals should print out values of NONSENSICAL.
static
TraversalResult printDtraceEntryAction(VariableEntry* var,
                                       char* varName,
                                       VariableOrigin varOrigin,
                                       UInt numDereferences,
                                       UInt layersBeforeBase,
                                       char overrideIsInit,
                                       DisambigOverride disambigOverride,
                                       char isSequence,
                                       // pValue only valid if isSequence is false
                                       void* pValue,
                                       // pValueArray and numElts only valid if
                                       // isSequence is true
                                       void** pValueArray,
                                       UInt numElts,
                                       FunctionEntry* varFuncInfo,
                                       char isEnter) {
  char variableHasBeenObserved = 0;
  void* firstInitElt = 0;

  char isHashcode = (layersBeforeBase > 0);

  // Line 1: Variable name
  DTRACE_PRINTF("%s\n", varName);

  // Lines 2 & 3: Value and modbit
  if (isSequence) {
    variableHasBeenObserved =
      printDtraceSequence(var,
                          pValueArray,
                          numElts,
                          varOrigin,
                          isHashcode,
                          disambigOverride,
                          &firstInitElt);
  }
  else {
    variableHasBeenObserved =
      printDtraceSingleVar(var,
                           pValue,
                           varOrigin,
                           isHashcode,
                           overrideIsInit,
                           disambigOverride);
  }

  // DynComp post-processing after observing a variable:
  if (kvasir_with_dyncomp && variableHasBeenObserved) {
    Addr a = 0;
    void* ptrInQuestion = 0;
    char ptrAllocAndInit = 0;

    // Pick the first initialized element from the sequence
    if (isSequence) {
      ptrInQuestion = firstInitElt;
    }
    else {
      ptrInQuestion = pValue;
    }

    // Special handling for static arrays: Currently, in the
    // .dtrace, for a static arrays 'int foo[]', we print out
    // 'foo' as the address of foo and 'foo[]' as the contents of
    // 'foo'.  However, for comparability, there is no place in
    // memory where the address of 'foo' is maintained; thus,
    // there is no tag for it anywhere, so we must not
    // post-process it and simply allow it to keep a tag of 0.
    // This implies that all static array hashcode values are
    // unique and not comparable to one another, which is the
    // intended behavior.  (Notice that if one wants to assign a
    // pointer to 'foo', then the address of 'foo' resides
    // somewhere in memory - where that pointer is located - and
    // thus gets a fresh tag.  One can then have that pointer
    // interact with other pointers and have THEM be comparable,
    // but 'foo' itself still has no tag and is not comparable to
    // anything else.)

    // Don't do anything if it's a base static array variable
    // (layersBeforeBase > 0) is okay since var->isStaticArray implies
    // that there is only one level of pointer indirection, and for a
    // static string (static array of 'char'), layersBeforeBase == 0
    // right away so we still process it
    if (!(var->isStaticArray &&
          (layersBeforeBase > 0))) {

      // Special handling for strings.  We are not interested in the
      // comparability of the 'char*' pointer variable, but rather
      // we are interested in the comparability of the CONTENTS of
      // the string.  (Be careful about statically-declared strings,
      // in which case the address of the first element is the address
      // of the pointer variable)
      if (var->isString &&
          (0 == layersBeforeBase)) {
        if (ptrInQuestion) {
          ptrAllocAndInit =
            (addressIsAllocated((Addr)ptrInQuestion, sizeof(void*)) &&
             addressIsInitialized((Addr)ptrInQuestion, sizeof(void*)));
        }

        if (ptrAllocAndInit) {
          // Depends on whether the variable is a static array or not:
          a = var->isStaticArray ?
            (Addr)ptrInQuestion :
            *((Addr*)(ptrInQuestion));
        }
      }
      else {
        a = (Addr)ptrInQuestion;
      }

      DC_post_process_for_variable((DaikonFunctionEntry*)varFuncInfo,
                                   isEnter,
                                   g_variableIndex,
                                   a);
    }
  }

  if (variableHasBeenObserved) {
    return DEREF_MORE_POINTERS;
  }
  else {
    return DO_NOT_DEREF_MORE_POINTERS;
  }
}


// Print an entry to the .dtrace file for an entrance or exit program
// point (determined by isEnter) of the function execution denoted by
// f_state:
void printDtraceForFunction(FunctionExecutionState* f_state, char isEnter) {
  FunctionEntry* funcPtr = 0;
  extern int g_variableIndex;

  tl_assert(f_state);
  funcPtr = f_state->func;
  tl_assert(funcPtr);

  //  VG_(printf)("* %s %s at EBP=0x%x, lowestESP=0x%x, startPC=%p\n",
              //              (isEnter ? "ENTER" : "EXIT "),
              //              f_state->func->fjalar_name,
              //              f_state->EBP,
              //              f_state->lowestESP,
              //              (void*)f_state->func->startPC);

  // Reset this properly!
  g_variableIndex = 0;


  // Print out function header
  if (!dyncomp_without_dtrace) {
    printDtraceFunctionHeader(funcPtr, isEnter);
  }


  // Print out globals:
  visitVariableGroup(GLOBAL_VAR,
                     funcPtr,
                     isEnter,
                     0,
                     &printDtraceEntryAction);

  // Print out function formal parameters:
  visitVariableGroup(FUNCTION_FORMAL_PARAM,
                     funcPtr,
                     isEnter,
                     // Remember to use the virtual stack!
                     f_state->virtualStack,
                     &printDtraceEntryAction);

  // If isEnter == 0, print out return value:
  if (!isEnter) {
    // Remember to use visitReturnValue() and NOT visitVariableGroup() here!
    visitReturnValue(f_state,
                     &printDtraceEntryAction);
  }


  // For debugging only - print out a .decls entry with all
  // comparability sets calculated thus far for this program point
  // after printing the .dtrace entry in order to allow all mergings
  // to occur:
  if (dyncomp_print_incremental && kvasir_with_dyncomp) {
    fputs("INTERMEDIATE ", decls_fp);
    printOneFunctionDecl(funcPtr, isEnter, 0);
    fflush(decls_fp);
  }

  // Flush the buffer so that everything for this program point gets
  // printed to the .dtrace file (useful for observing executions of
  // interactive programs):
  if (dtrace_fp) {
    fflush(dtrace_fp);
  }

  // If --dyncomp-detailed-mode is on, at this point we have collected
  // all of the leader tags of the values of all Daikon variables
  // during a certain program point execution, so we can process them
  // all in an O(n^2) manner to mutate bitmatrix.
  if (kvasir_with_dyncomp && dyncomp_detailed_mode) {
    DC_detailed_mode_process_ppt_execution(funcPtr, isEnter);
  }
}
