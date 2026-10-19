/**
 * @file libtcc.h
 * @brief Embedded TinyCC-compatible C Compiler API for the Cortez Kernel.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct TCCState;
typedef struct TCCState TCCState;

typedef void (*TCCErrorFunc)(void* opaque, const char* msg);

#define TCC_OUTPUT_MEMORY     1
#define TCC_OUTPUT_EXE        2
#define TCC_OUTPUT_DLL        3
#define TCC_OUTPUT_OBJ        4
#define TCC_OUTPUT_PREPROCESS 5

#define TCC_RELOCATE_AUTO     ((void*)1)

/* Lifecycle & configuration */
TCCState* tcc_new(void);
void      tcc_delete(TCCState* s);
void      tcc_set_lib_path(TCCState* s, const char* path);
void      tcc_set_error_func(TCCState* s, void* error_opaque, TCCErrorFunc error_func);
int       tcc_set_options(TCCState* s, const char* str);

/* Preprocessor configuration */
int       tcc_add_include_path(TCCState* s, const char* pathname);
int       tcc_add_sysinclude_path(TCCState* s, const char* pathname);
void      tcc_define_symbol(TCCState* s, const char* sym, const char* value);
void      tcc_undefine_symbol(TCCState* s, const char* sym);

/* Compilation */
int       tcc_add_file(TCCState* s, const char* filename);
int       tcc_compile_string(TCCState* s, const char* buf);

/* Linking & in-memory relocation */
int       tcc_set_output_type(TCCState* s, int output_type);
int       tcc_output_file(TCCState* s, const char* filename);
int       tcc_add_Symbol(TCCState* s, const char* name, const void* val);
int       tcc_add_symbol(TCCState* s, const char* name, const void* val);
int       tcc_relocate(TCCState* s1, void* ptr);
void*     tcc_get_symbol(TCCState* s, const char* name);

/* Cortez Kernel symbol introspection for crash backtraces (Phase 6) */
size_t      tcc_get_exported_symbol_count(TCCState* s);
const char* tcc_get_exported_symbol_name(TCCState* s, size_t index);
void*       tcc_get_exported_symbol_addr(TCCState* s, size_t index);

#ifdef __cplusplus
}
#endif
