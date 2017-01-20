/* C/C++ language support for compilation.

   Copyright (C) 2014-2017 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "compile-internal.h"
#include "compile-c.h"
#include "compile-cplus.h"
#include "compile.h"
#include "gdb-dlfcn.h"
#include "c-lang.h"
#include "macrotab.h"
#include "macroscope.h"
#include "regcache.h"

/* See compile-internal.h.  */

const char *
c_get_mode_for_size (int size)
{
  const char *mode = NULL;

  switch (size)
    {
    case 1:
      mode = "QI";
      break;
    case 2:
      mode = "HI";
      break;
    case 4:
      mode = "SI";
      break;
    case 8:
      mode = "DI";
      break;
    default:
      internal_error (__FILE__, __LINE__, _("Invalid GCC mode size %d."), size);
    }

  return mode;
}

/* See compile-internal.h.  */

char *
c_get_range_decl_name (const struct dynamic_prop *prop)
{
  return xstrprintf ("__gdb_prop_%s", host_address_to_string (prop));
}

#define STR(x) #x
#define STRINGIFY(x) STR(x)

/* Load the plug-in library FE_LIBCC and return the initialization function
   FE_CONTEXT.  */

template <typename FUNCTYPE>
FUNCTYPE *
load_libcompile (const char *fe_libcc, const char *fe_context)
{
  void *handle;
  FUNCTYPE *func;

  /* gdb_dlopen will call error () on an error, so no need to check
     value.  */
  handle = gdb_dlopen (fe_libcc);
  func = (FUNCTYPE *) (gdb_dlsym (handle, fe_context));

  if (func == NULL)
    error (_("could not find symbol %s in library %s"), fe_context, fe_libcc);
  return func;
}

/* Return the compile instance associated with the current context.
   This function calls the symbol returned from the load_libcompile
   function.  FE_LIBCC is the library to load.  BASE_VERSION is the
   base compile plug-in version we support.  API_VERSION is the
   API version supported.  */

template <typename INSTTYPE, typename FUNCTYPE, typename CTXTYPE,
	  typename BASE_VERSION_TYPE, typename API_VERSION_TYPE>
struct compile_instance *
get_compile_context (const char *fe_libcc, const char *fe_context,
		     BASE_VERSION_TYPE base_version,
		     API_VERSION_TYPE api_version)
{
  static FUNCTYPE *func;
  static CTXTYPE *context;

  if (func == NULL)
    {
      func = load_libcompile<FUNCTYPE> (fe_libcc, fe_context);
      gdb_assert (func != NULL);
    }

  context = (*func) (base_version, api_version);
  if (context == NULL)
    error (_("The loaded version of GCC does not support the required version "
	     "of the API."));

  return new INSTTYPE (context);
}

/* A C-language implementation of get_compile_context.  */

struct compile_instance *
c_get_compile_context (void)
{
  return get_compile_context
    <compile_c_instance, gcc_c_fe_context_function, gcc_c_context,
    gcc_base_api_version, gcc_c_api_version>
    (STRINGIFY (GCC_C_FE_LIBCC), STRINGIFY (GCC_C_FE_CONTEXT),
     GCC_FE_VERSION_0, GCC_C_FE_VERSION_0);
}

/* A C++-language implementation of get_compile_context.  */

struct compile_instance *
cplus_get_compile_context (void)
{
  using namespace compile;

  return get_compile_context
    <compile_cplus_instance, gcc_cp_fe_context_function, gcc_cp_context,
     gcc_base_api_version, gcc_cp_api_version>
    (STRINGIFY (GCC_CP_FE_LIBCC), STRINGIFY (GCC_CP_FE_CONTEXT),
     GCC_FE_VERSION_0, GCC_CP_FE_VERSION_0);
}



/* Write one macro definition.  */

static void
print_one_macro (const char *name, const struct macro_definition *macro,
		 struct macro_source_file *source, int line,
		 void *user_data)
{
  struct ui_file *file = (struct ui_file *) user_data;

  /* Don't print command-line defines.  They will be supplied another
     way.  */
  if (line == 0)
    return;

  /* None of -Wno-builtin-macro-redefined, #undef first
     or plain #define of the same value would avoid a warning.  */
  fprintf_filtered (file, "#ifndef %s\n# define %s", name, name);

  if (macro->kind == macro_function_like)
    {
      int i;

      fputs_filtered ("(", file);
      for (i = 0; i < macro->argc; i++)
	{
	  fputs_filtered (macro->argv[i], file);
	  if (i + 1 < macro->argc)
	    fputs_filtered (", ", file);
	}
      fputs_filtered (")", file);
    }

  fprintf_filtered (file, " %s\n#endif\n", macro->replacement);
}

/* Write macro definitions at PC to FILE.  */

static void
write_macro_definitions (const struct block *block, CORE_ADDR pc,
			 struct ui_file *file)
{
  struct macro_scope *scope;

  if (block != NULL)
    scope = sal_macro_scope (find_pc_line (pc, 0));
  else
    scope = default_macro_scope ();
  if (scope == NULL)
    scope = user_macro_scope ();

  if (scope != NULL && scope->file != NULL && scope->file->table != NULL)
    macro_for_each_in_scope (scope->file, scope->line, print_one_macro, file);
}


/* Generate a structure holding all the registers used by the function
   we're generating.  */

static void
generate_register_struct (struct ui_file *stream, struct gdbarch *gdbarch,
			  const unsigned char *registers_used)
{
  int seen = 0;

  fputs_unfiltered ("struct " COMPILE_I_SIMPLE_REGISTER_STRUCT_TAG " {\n",
		    stream);

  if (registers_used != NULL)
    for (int i = 0; i < gdbarch_num_regs (gdbarch); ++i)
      {
	if (registers_used[i])
	  {
	    struct type *regtype = check_typedef (register_type (gdbarch, i));
	    char *regname = compile_register_name_mangled (gdbarch, i);
	    struct cleanup *cleanups = make_cleanup (xfree, regname);

	    seen = 1;

	    /* You might think we could use type_print here.  However,
	       target descriptions often use types with names like
	       "int64_t", which may not be defined in the inferior
	       (and in any case would not be looked up due to the
	       #pragma business).  So, we take a much simpler
	       approach: for pointer- or integer-typed registers, emit
	       the field in the most direct way; and for other
	       register types (typically flags or vectors), emit a
	       maximally-aligned array of the correct size.  */

	    fputs_unfiltered ("  ", stream);
	    switch (TYPE_CODE (regtype))
	      {
	      case TYPE_CODE_PTR:
		fprintf_filtered (stream, "__gdb_uintptr %s", regname);
		break;

	      case TYPE_CODE_INT:
		{
		  const char *mode
		    = c_get_mode_for_size (TYPE_LENGTH (regtype));

		  if (mode != NULL)
		    {
		      if (TYPE_UNSIGNED (regtype))
			fputs_unfiltered ("unsigned ", stream);
		      fprintf_unfiltered (stream,
					  "int %s"
					  " __attribute__ ((__mode__(__%s__)))",
					  regname,
					  mode);
		      break;
		    }
		}

		/* Fall through.  */

	      default:
		fprintf_unfiltered (stream,
				    "  unsigned char %s[%d]"
				    " __attribute__((__aligned__("
				    "__BIGGEST_ALIGNMENT__)))",
				    regname,
				    TYPE_LENGTH (regtype));
	      }
	    fputs_unfiltered (";\n", stream);

	    do_cleanups (cleanups);
	  }
      }

  if (!seen)
    fputs_unfiltered ("  char " COMPILE_I_SIMPLE_REGISTER_DUMMY ";\n",
		      stream);

  fputs_unfiltered ("};\n\n", stream);
}

/* C-language policy to emit a push user expression pragma into BUF.  */

struct c_push_user_expression
{
  void push_user_expression (struct ui_file *buf)
  {
    fputs_unfiltered ("#pragma GCC user_expression\n", buf);
  }
};

/* C-language policy to emit a pop user expression pragma into BUF.
   For C, this is a nop.  */

struct pop_user_expression_nop
{
  void pop_user_expression (struct ui_file *buf)
  {
    /* Nothing to do.  */
  }
};

/* C-language policy to construct a code header for a block of code.
   Takes a scope TYPE argument which selects the correct header to
   insert into BUF.  */

struct c_add_code_header
{
  void add_code_header (enum compile_i_scope_types type, struct ui_file *buf)
  {
    switch (type)
      {
      case COMPILE_I_SIMPLE_SCOPE:
	fputs_unfiltered ("void "
			  GCC_FE_WRAPPER_FUNCTION
			  " (struct "
			  COMPILE_I_SIMPLE_REGISTER_STRUCT_TAG
			  " *"
			  COMPILE_I_SIMPLE_REGISTER_ARG_NAME
			  ") {\n",
			  buf);
	break;

      case COMPILE_I_PRINT_ADDRESS_SCOPE:
      case COMPILE_I_PRINT_VALUE_SCOPE:
	/* <string.h> is needed for a memcpy call below.  */
	fputs_unfiltered ("#include <string.h>\n"
			  "void "
			  GCC_FE_WRAPPER_FUNCTION
			  " (struct "
			  COMPILE_I_SIMPLE_REGISTER_STRUCT_TAG
			  " *"
			  COMPILE_I_SIMPLE_REGISTER_ARG_NAME
			  ", "
			  COMPILE_I_PRINT_OUT_ARG_TYPE
			  " "
			  COMPILE_I_PRINT_OUT_ARG
			  ") {\n",
			  buf);
	break;

      case COMPILE_I_RAW_SCOPE:
	break;

      default:
	gdb_assert_not_reached (_("Unknown compiler scope reached."));
      }
  }
};

/* C-language policy to construct a code footer for a block of code.
   Takes a scope TYPE which selects the correct footer to insert into BUF.  */

struct c_add_code_footer
{
  void add_code_footer (enum compile_i_scope_types type, struct ui_file *buf)
  {
    switch (type)
      {
      case COMPILE_I_SIMPLE_SCOPE:
      case COMPILE_I_PRINT_ADDRESS_SCOPE:
      case COMPILE_I_PRINT_VALUE_SCOPE:
	fputs_unfiltered ("}\n", buf);
	break;

      case COMPILE_I_RAW_SCOPE:
	break;

      default:
	gdb_assert_not_reached (_("Unknown compiler scope reached."));
      }
  }
};

/* C-language policy to emit the user code snippet INPUT into BUF based on the
   scope TYPE.  */

struct c_add_input
{
  void add_input (enum compile_i_scope_types type, const char *input,
		  struct ui_file *buf)
  {
    switch (type)
      {
      case COMPILE_I_PRINT_ADDRESS_SCOPE:
      case COMPILE_I_PRINT_VALUE_SCOPE:
	fprintf_unfiltered (buf,
			    "__auto_type " COMPILE_I_EXPR_VAL " = %s;\n"
			    "typeof (%s) *" COMPILE_I_EXPR_PTR_TYPE ";\n"
			    "memcpy (" COMPILE_I_PRINT_OUT_ARG ", %s"
			    COMPILE_I_EXPR_VAL ",\n"
			    "sizeof (*" COMPILE_I_EXPR_PTR_TYPE "));\n"
			    , input, input,
			    (type == COMPILE_I_PRINT_ADDRESS_SCOPE
			     ? "&" : ""));
	break;

      default:
	fputs_unfiltered (input, buf);
	break;
      }
    fputs_unfiltered ("\n", buf);
  }
};

/* C++-language policy to emit a push user expression pragma into
   BUF.  */

struct cplus_push_user_expression
{
  void push_user_expression (struct ui_file *buf)
  {
    fputs_unfiltered ("#pragma GCC push_user_expression\n", buf);
  }
};

/* C++-language policy to emit a pop user expression pragma into BUF.  */

struct cplus_pop_user_expression
{
  void pop_user_expression (struct ui_file *buf)
  {
    fputs_unfiltered ("#pragma GCC pop_user_expression\n", buf);
  }
};

/* C++-language policy to construct a code header for a block of code.
   Takes a scope TYPE argument which selects the correct header to
   insert into BUF.  */

struct cplus_add_code_header
{
  void add_code_header (enum compile_i_scope_types type, struct ui_file *buf)
  {
  switch (type)
    {
    case COMPILE_I_SIMPLE_SCOPE:
      fputs_unfiltered ("void "
			GCC_FE_WRAPPER_FUNCTION
			" (struct "
			COMPILE_I_SIMPLE_REGISTER_STRUCT_TAG
			" *"
			COMPILE_I_SIMPLE_REGISTER_ARG_NAME
			") {\n",
			buf);
      break;

    case COMPILE_I_PRINT_ADDRESS_SCOPE:
    case COMPILE_I_PRINT_VALUE_SCOPE:
      fputs_unfiltered (
			"#include <cstring>\n"
			"#include <bits/move.h>\n"
			"void "
			GCC_FE_WRAPPER_FUNCTION
			" (struct "
			COMPILE_I_SIMPLE_REGISTER_STRUCT_TAG
			" *"
			COMPILE_I_SIMPLE_REGISTER_ARG_NAME
			", "
			COMPILE_I_PRINT_OUT_ARG_TYPE
			" "
			COMPILE_I_PRINT_OUT_ARG
			") {\n",
			buf);
      break;

    case COMPILE_I_RAW_SCOPE:
      break;

    default:
      gdb_assert_not_reached (_("Unknown compiler scope reached."));
    }
  }
};

/* C++-language policy to emit the user code snippet INPUT into BUF based on
   the scope TYPE.  */

struct cplus_add_input
{
  void add_input (enum compile_i_scope_types type, const char *input,
		  struct ui_file *buf)
  {
    switch (type)
      {
      case COMPILE_I_PRINT_ADDRESS_SCOPE:
      case COMPILE_I_PRINT_VALUE_SCOPE:
	fprintf_unfiltered
	  (buf,
	   "auto " COMPILE_I_EXPR_VAL " = %s;\n"
	   "decltype ( %s ) *" COMPILE_I_EXPR_PTR_TYPE ";\n"
	   "std::memcpy (" COMPILE_I_PRINT_OUT_ARG ", %s ("
	   COMPILE_I_EXPR_VAL "),\n"
	   "sizeof (decltype(%s)));\n"
	   ,input, input,
	   (type == COMPILE_I_PRINT_ADDRESS_SCOPE
	    ? "std::__addressof" : ""), input);
	break;

      default:
	fputs_unfiltered (input, buf);
	break;
      }
    fputs_unfiltered ("\n", buf);
  }
};

/* A host class representing a compile program.

   CompileInstanceType is the type of the compile_instance for the
   langauge.

   PushUserExpressionPolicy and PopUserExpressionPolicy are used to
   push and pop user expression pragmas to the compile plug-in.

   AddCodeHeaderPolicy and AddCodeFooterPolicy are used to add the appropriate
   code header and footer, respectively.

   AddInputPolicy adds the actual user code.  */

template <class CompileInstanceType, class PushUserExpressionPolicy,
	  class PopUserExpressionPolicy, class AddCodeHeaderPolicy,
	  class AddCodeFooterPolicy, class AddInputPolicy>
class compile_program
  : private PushUserExpressionPolicy, private PopUserExpressionPolicy,
    private AddCodeHeaderPolicy, private AddCodeFooterPolicy,
    private AddInputPolicy
{
  using PushUserExpressionPolicy::push_user_expression;
  using PopUserExpressionPolicy::pop_user_expression;
  using AddCodeHeaderPolicy::add_code_header;
  using AddCodeFooterPolicy::add_code_footer;
  using AddInputPolicy::add_input;

public:

  /* Construct a compile_program using the compiler instance INST
     using the architecture given by GDBARCH.  */

  compile_program (CompileInstanceType *inst, struct gdbarch *gdbarch)
    : m_instance (inst), m_arch (gdbarch)
  {
  }

  /* Take the source code provided by the user with the 'compile'
     command and compute the additional wrapping, macro, variable and
     register operations needed.  INPUT is the source code derived from
     the 'compile' command, EXPR_BLOCK denotes the block relevant contextually
     to the inferior when the expression was created, and EXPR_PC
     indicates the value of $PC.

     Returns the text of the program to compile.  This must be free'd by
     the caller.  */

  std::string compute (const char *input, const struct block *expr_block,
		       CORE_ADDR expr_pc)
  {
    struct ui_file *var_stream = NULL;
    struct ui_file *buf = mem_fileopen ();
    struct cleanup *cleanup = make_cleanup_ui_file_delete (buf);

    /* Do not generate local variable information for "raw"
       compilations.  In this case we aren't emitting our own function
       and the user's code may only refer to globals.  */
    if (m_instance->scope () != COMPILE_I_RAW_SCOPE)
      {
	/* Generate the code to compute variable locations, but do it
	   before generating the function header, so we can define the
	   register struct before the function body.  This requires a
	   temporary stream.  */
	var_stream = mem_fileopen ();
	make_cleanup_ui_file_delete (var_stream);
	unsigned char *registers_used
	  = generate_c_for_variable_locations (m_instance, var_stream, m_arch,
					       expr_block, expr_pc);
	make_cleanup (xfree, registers_used);

	fputs_unfiltered ("typedef unsigned int"
			  " __attribute__ ((__mode__(__pointer__)))"
			  " __gdb_uintptr;\n",
			  buf);
	fputs_unfiltered ("typedef int"
			  " __attribute__ ((__mode__(__pointer__)))"
			  " __gdb_intptr;\n",
			  buf);

	/* Iterate all log2 sizes in bytes supported by c_get_mode_for_size.  */
	for (int i = 0; i < 4; ++i)
	  {
	    const char *mode = c_get_mode_for_size (1 << i);

	    gdb_assert (mode != NULL);
	    fprintf_unfiltered (buf,
				"typedef int"
				" __attribute__ ((__mode__(__%s__)))"
				" __gdb_int_%s;\n",
				mode, mode);
	  }

	generate_register_struct (buf, m_arch, registers_used);
      }

    add_code_header (m_instance->scope (), buf);

    if (m_instance->scope () == COMPILE_I_SIMPLE_SCOPE
	|| m_instance->scope () == COMPILE_I_PRINT_ADDRESS_SCOPE
	|| m_instance->scope () == COMPILE_I_PRINT_VALUE_SCOPE)
      {
	ui_file_put (var_stream, ui_file_write_for_put, buf);
	push_user_expression (buf);
      }

    write_macro_definitions (expr_block, expr_pc, buf);

    /* The user expression has to be in its own scope, so that "extern"
       works properly.  Otherwise gcc thinks that the "extern"
       declaration is in the same scope as the declaration provided by
       gdb.  */
    if (m_instance->scope () != COMPILE_I_RAW_SCOPE)
      fputs_unfiltered ("{\n", buf);

    fputs_unfiltered ("#line 1 \"gdb command line\"\n", buf);

    add_input (m_instance->scope (), input, buf);

    /* For larger user expressions the automatic semicolons may be
       confusing.  */
    if (strchr (input, '\n') == NULL)
      fputs_unfiltered (";\n", buf);

    if (m_instance->scope () != COMPILE_I_RAW_SCOPE)
      fputs_unfiltered ("}\n", buf);

    if (m_instance->scope () == COMPILE_I_SIMPLE_SCOPE
	|| m_instance->scope () == COMPILE_I_PRINT_ADDRESS_SCOPE
	|| m_instance->scope () == COMPILE_I_PRINT_VALUE_SCOPE)
      pop_user_expression (buf);

    add_code_footer (m_instance->scope (), buf);
    std::string code = ui_file_as_string (buf);
    do_cleanups (cleanup);
    return code;
  }

private:

  /* The compile instance to be used for compilation and
     type-conversion.  */
  CompileInstanceType *m_instance;

  /* The architecture to be used.  */
  struct gdbarch *m_arch;
};

/* The types used for C and C++ program computations.  */

typedef compile_program<compile_c_instance, c_push_user_expression,
			pop_user_expression_nop, c_add_code_header,
			c_add_code_footer,
			c_add_input> c_compile_program;

typedef compile_program<compile::compile_cplus_instance,
			cplus_push_user_expression, cplus_pop_user_expression,
			cplus_add_code_header, c_add_code_footer,
			cplus_add_input> cplus_compile_program;

/* The la_compute_program method for C.  */

std::string
c_compute_program (struct compile_instance *inst,
		   const char *input,
		   struct gdbarch *gdbarch,
		   const struct block *expr_block,
		   CORE_ADDR expr_pc)
{
  compile_c_instance *c_inst = static_cast<compile_c_instance *> (inst);
  c_compile_program program (c_inst, gdbarch);

  return program.compute (input, expr_block, expr_pc);
}

/* The la_compute_program method for C++.  */

std::string
cplus_compute_program (struct compile_instance *inst,
		       const char *input,
		       struct gdbarch *gdbarch,
		       const struct block *expr_block,
		       CORE_ADDR expr_pc)
{
  using namespace compile;

  compile_cplus_instance *cplus_inst
    = static_cast<compile_cplus_instance *> (inst);
  cplus_compile_program program (cplus_inst, gdbarch);

  return program.compute (input, expr_block, expr_pc);
}
