/* GNU m4 -- A simple macro processor
   Copyright 1989, 1990, 1991, 1992, 1993, 1994, 2001
   Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307  USA
*/

/* This file contains the functions that perform the basic argument
   parsing and macro expansion.  */

#include "m4.h"
#include "m4private.h"

static void    collect_arguments (m4 *context, const char *name,
				  m4_symbol *symbol, struct obstack *argptr,
				  struct obstack *arguments);
static void    expand_macro      (m4 *context, const char *name,
				  m4_symbol *symbol);
static void    expand_token      (m4 *context, struct obstack *obs,
				  m4__token_type type, m4_symbol_value *token);
static boolean expand_argument   (m4 *context, struct obstack *obs,
				  m4_symbol_value *argp);

/* Current recursion level in expand_macro ().  */
int m4_expansion_level = 0;

/* The number of the current call of expand_macro ().  */
static int macro_call_id = 0;

/* This function reads all input, and expands each token, one at a time.  */
void
m4_expand_input (m4 *context)
{
  m4__token_type type;
  m4_symbol_value token;

  while ((type = m4__next_token (context, &token)) != M4_TOKEN_EOF)
    expand_token (context, (struct obstack *) NULL, type, &token);
}


/* Expand one token, according to its type.  Potential macro names
   (M4_TOKEN_WORD) are looked up in the symbol table, to see if they have a
   macro definition.  If they have, they are expanded as macros, otherwise
   the text are just copied to the output.  */
static void
expand_token (m4 *context, struct obstack *obs,
	      m4__token_type type, m4_symbol_value *token)
{
  m4_symbol *symbol;
  char *text = xstrdup (m4_get_symbol_value_text (token));

  switch (type)
    {				/* TOKSW */
    case M4_TOKEN_EOF:
    case M4_TOKEN_MACDEF:
      break;

    case M4_TOKEN_SIMPLE:
    case M4_TOKEN_STRING:
    case M4_TOKEN_SPACE:
      m4_shipout_text (context, obs, text, strlen (text));
      break;

    case M4_TOKEN_WORD:
      {
	char *textp = text;

	if (M4_IS_ESCAPE (M4SYNTAX, *textp))
	  ++textp;

	symbol = m4_symbol_lookup (M4SYMTAB, textp);
	if (symbol == NULL
	    || symbol->value->type == M4_SYMBOL_VOID
	    || (symbol->value->type == M4_SYMBOL_FUNC
		&& BIT_TEST (SYMBOL_FLAGS (symbol), VALUE_BLIND_ARGS_BIT)
		&& !M4_IS_OPEN (M4SYNTAX, m4_peek_input (context))))
	  {
	    m4_shipout_text (context, obs, text, strlen (text));
	  }
	else
	  expand_macro (context, textp, symbol);
      }
      break;

    default:
      M4ERROR ((m4_get_warning_status_opt (context), 0,
		"INTERNAL ERROR: Bad token type in expand_token ()"));
      abort ();
    }

  XFREE (text);
}


/* This function parses one argument to a macro call.  It expects the first
   left parenthesis, or the separating comma to have been read by the
   caller.  It skips leading whitespace, and reads and expands tokens,
   until it finds a comma or a right parenthesis at the same level of
   parentheses.  It returns a flag indicating whether the argument read is
   the last for the active macro call.  The arguments are built on the
   obstack OBS, indirectly through expand_token ().	 */
static boolean
expand_argument (m4 *context, struct obstack *obs, m4_symbol_value *argp)
{
  m4__token_type type;
  m4_symbol_value token;
  char *text;
  int paren_level = 0;
  const char *current_file = m4_current_file;
  int current_line = m4_current_line;

  argp->type = M4_SYMBOL_VOID;

  /* Skip leading white space.  */
  do
    {
      type = m4__next_token (context, &token);
    }
  while (type == M4_TOKEN_SPACE);

  while (1)
    {
      switch (type)
	{			/* TOKSW */
	case M4_TOKEN_SIMPLE:
	  text = m4_get_symbol_value_text (&token);
	  if ((M4_IS_COMMA (M4SYNTAX, *text) || M4_IS_CLOSE (M4SYNTAX, *text))
	      && paren_level == 0)
	    {

	      /* The argument MUST be finished, whether we want it or not.  */
	      obstack_1grow (obs, '\0');
	      text = obstack_finish (obs);

	      if (argp->type == M4_SYMBOL_VOID)
		{
		  m4_set_symbol_value_text (argp, text);
		}
	      return (boolean) (M4_IS_COMMA (M4SYNTAX, *m4_get_symbol_value_text (&token)));
	    }

	  if (M4_IS_OPEN (M4SYNTAX, *text))
	    paren_level++;
	  else if (M4_IS_CLOSE (M4SYNTAX, *text))
	    paren_level--;
	  expand_token (context, obs, type, &token);
	  break;

	case M4_TOKEN_EOF:
	  error_at_line (EXIT_FAILURE, 0, current_file, current_line,
			 _("EOF in argument list"));
	  break;

	case M4_TOKEN_WORD:
	case M4_TOKEN_SPACE:
	case M4_TOKEN_STRING:
	  expand_token (context, obs, type, &token);
	  break;

	case M4_TOKEN_MACDEF:
	  if (obstack_object_size (obs) == 0)
	    m4_symbol_value_copy (argp, &token);
	  break;

	default:
	  M4ERROR ((m4_get_warning_status_opt (context), 0,
		    "INTERNAL ERROR: Bad token type in expand_argument ()"));
	  abort ();
	}

      type = m4__next_token (context, &token);
    }
}


/* The macro expansion is handled by expand_macro ().  It parses the
   arguments, using collect_arguments (), and builds a table of pointers to
   the arguments.  The arguments themselves are stored on a local obstack.
   Expand_macro () uses call_macro () to do the call of the macro.

   Expand_macro () is potentially recursive, since it calls expand_argument
   (), which might call expand_token (), which might call expand_macro ().  */
static void
expand_macro (m4 *context, const char *name, m4_symbol *symbol)
{
  struct obstack arguments;
  struct obstack argptr;
  m4_symbol_value **argv;
  int argc;
  struct obstack *expansion;
  const char *expanded;
  boolean traced;
  int my_call_id;

  m4_expansion_level++;
  if (m4_expansion_level > m4_get_nesting_limit_opt (context))
    M4ERROR ((EXIT_FAILURE, 0, _("\
ERROR: Recursion limit of %d exceeded, use -L<N> to change it"),
	      m4_get_nesting_limit_opt (context)));

  macro_call_id++;
  my_call_id = macro_call_id;

  traced = (boolean) ((BIT_TEST (m4_get_debug_level_opt (context),
				 M4_DEBUG_TRACE_ALL))
		      || m4_get_symbol_traced (symbol));

  obstack_init (&argptr);
  obstack_init (&arguments);

  if (traced && (BIT_TEST (m4_get_debug_level_opt (context),
			   M4_DEBUG_TRACE_CALL)))
    m4_trace_prepre (context, name, my_call_id);

  collect_arguments (context, name, symbol, &argptr, &arguments);

  argc = obstack_object_size (&argptr) / sizeof (m4_symbol_value *);
  argv = (m4_symbol_value **) obstack_finish (&argptr);

  if (traced)
    m4_trace_pre (context, name, my_call_id, argc, argv);

  expansion = m4_push_string_init (context);
  if (!m4_bad_argc (context, argc, argv,
		    SYMBOL_MIN_ARGS (symbol), SYMBOL_MAX_ARGS (symbol)))
    m4_call_macro (context, symbol, expansion, argc, argv);
  expanded = m4_push_string_finish ();

  if (traced)
    m4_trace_post (context, name, my_call_id, argc, argv, expanded);

  --m4_expansion_level;

  obstack_free (&arguments, NULL);
  obstack_free (&argptr, NULL);
}

/* Collect all the arguments to a call of the macro SYMBOL (called NAME).
   The arguments are stored on the obstack ARGUMENTS and a table of pointers
   to the arguments on the obstack ARGPTR.  */
static void
collect_arguments (m4 *context, const char *name, m4_symbol *symbol,
		   struct obstack *argptr, struct obstack *arguments)
{
  int ch;			/* lookahead for ( */
  m4_symbol_value token;
  m4_symbol_value *tokenp;
  boolean more_args;
  boolean groks_macro_args;

  groks_macro_args = BIT_TEST (SYMBOL_FLAGS (symbol), VALUE_MACRO_ARGS_BIT);

  m4_set_symbol_value_text (&token, (char *) name);
  tokenp = (m4_symbol_value *) obstack_copy (arguments, (void *) &token,
				      sizeof (token));
  obstack_grow (argptr, (void *) &tokenp, sizeof (tokenp));

  ch = m4_peek_input (context);
  if (M4_IS_OPEN (M4SYNTAX, ch))
    {
      m4__next_token (context, &token);		/* gobble parenthesis */
      do
	{
	  more_args = expand_argument (context, arguments, &token);

	  if (!groks_macro_args && m4_is_symbol_value_func (&token))
	    {
	      m4_set_symbol_value_text (&token, "");
	    }
	  tokenp = (m4_symbol_value *)
	    obstack_copy (arguments, (void *) &token, sizeof (token));
	  obstack_grow (argptr, (void *) &tokenp, sizeof (tokenp));
	}
      while (more_args);
    }
}


/* The actual call of a macro is handled by m4_call_macro ().
   m4_call_macro () is passed a SYMBOL, whose type is used to
   call either a builtin function, or the user macro expansion
   function m4_process_macro ().  There are ARGC arguments to
   the call, stored in the ARGV table.  The expansion is left on
   the obstack EXPANSION.  Macro tracing is also handled here.  */
void
m4_call_macro (m4 *context, m4_symbol *symbol, struct obstack *expansion,
	       int argc, m4_symbol_value **argv)
{
  if (m4_is_symbol_text (symbol))
    {
      m4_process_macro (context, symbol, expansion, argc, argv);
    }
  else if (m4_is_symbol_func (symbol))
    {
      (*m4_get_symbol_func (symbol)) (context, expansion, argc, argv);
    }
  else
    {
      M4ERROR ((m4_get_warning_status_opt (context), 0,
		"INTERNAL ERROR: Bad symbol type in call_macro ()"));
      abort ();
    }
}

/* This function handles all expansion of user defined and predefined
   macros.  It is called with an obstack OBS, where the macros expansion
   will be placed, as an unfinished object.  SYMBOL points to the macro
   definition, giving the expansion text.  ARGC and ARGV are the arguments,
   as usual.  */
void
m4_process_macro (m4 *context, m4_symbol *symbol, struct obstack *obs,
		  int argc, m4_symbol_value **argv)
{
  const unsigned char *text;
  int i;

  for (text = m4_get_symbol_text (symbol); *text != '\0';)
    {
      char ch;

      if (!M4_IS_DOLLAR (M4SYNTAX, *text))
	{
	  obstack_1grow (obs, *text);
	  text++;
	  continue;
	}
      ch = *text++;
      switch (*text)
	{
	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9':
	  if (m4_get_no_gnu_extensions_opt (context) || !isdigit(text[1]))
	    {
	      i = *text++ - '0';
	    }
	  else
	    {
	      char *endp;
	      i = (int)strtol (text, &endp, 10);
	      text = endp;
	    }
	  if (i < argc)
	    m4_shipout_string (context, obs, M4ARG (i), 0, FALSE);
	  break;

	case '#':		/* number of arguments */
	  m4_shipout_int (obs, argc - 1);
	  text++;
	  break;

	case '*':		/* all arguments */
	case '@':		/* ... same, but quoted */
	  m4_dump_args (context, obs, argc, argv, ",", *text == '@');
	  text++;
	  break;

	default:
	  if (m4_get_no_gnu_extensions_opt (context)
	      || !SYMBOL_ARG_SIGNATURE (symbol))
	    {
	      obstack_1grow (obs, ch);
	    }
	  else
	    {
	      size_t       len  = 0;
	      const char * endp;
	      const char * key;

	      for (endp = ++text; *endp && M4_IS_IDENT (M4SYNTAX, *endp); ++endp)
		++len;
	      key = xstrzdup (text, len);

	      if (*endp)
		{
		  struct m4_symbol_arg **arg
		    = (struct m4_symbol_arg **)
		      m4_hash_lookup (SYMBOL_ARG_SIGNATURE (symbol), key);

		  if (arg)
		    {
		      i = SYMBOL_ARG_INDEX (*arg);

		      if (i < argc)
			m4_shipout_string (context, obs, M4ARG (i), 0, FALSE);
		      else
			M4ERROR ((EXIT_FAILURE, 0, "\
INTERNAL ERROR: %s: out of range reference `%d' from argument %s",
			      M4ARG (0), i, key));
		    }
		}
	      else
		{
		  M4ERROR ((m4_get_warning_status_opt (context), 0,
			  _("Error: %s: unterminated parameter reference: %s"),
			    M4ARG (0), key));
		}

	      text = *endp ? 1+ endp : endp;

	      xfree ((char *) key);
	      break;
	    }
	  break;
	}
    }
}
