/*
  +----------------------------------------------------------------------+
  | Suhosin Version 1                                                    |
  +----------------------------------------------------------------------+
  | Copyright (c) 2006-2007 The Hardened-PHP Project                     |
  | Copyright (c) 2007-2015 SektionEins GmbH                             |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Stefan Esser <sesser@sektioneins.de>                         |
  +----------------------------------------------------------------------+
*/

/* $Id: execute.c,v 1.1.1.1 2007-11-28 01:15:35 sesser Exp $ */
// #if 0
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <fcntl.h>
#include "php.h"
#include "php_ini.h"
#include "zend_hash.h"
#include "zend_extensions.h"
#include "ext/standard/info.h"
#include "ext/standard/php_rand.h"
#include "ext/standard/php_lcg.h"
#include "php_suhosin7.h"
#include "zend_compile.h"
#include "zend_llist.h"
#include "SAPI.h"

#include "sha256.h"

#ifdef PHP_WIN32
# include "win32/fnmatch.h"
# include "win32/winutil.h"
# include "win32/time.h"
#else
# ifdef HAVE_FNMATCH
#  include <fnmatch.h>
# endif
# include <sys/time.h>
#endif

ZEND_API static void (*old_execute_ex)(zend_execute_data *execute_data);
ZEND_API static void suhosin_execute_ex(zend_execute_data *execute_data);
ZEND_API static void (*old_execute_internal)(zend_execute_data *execute_data, zval *return_value);
ZEND_API static void suhosin_execute_internal(zend_execute_data *execute_data, zval *return_value);
ZEND_API static void (*old_execute)(zend_op_array *op_array, zval *return_value);
ZEND_API static void suhosin_execute(zend_op_array *op_array, zval *return_value);
// static void (*old_execute_ZO)(zend_op_array *op_array, long dummy);
// static void suhosin_execute_ZO(zend_op_array *op_array, long dummy);
// static void *(*zo_set_oe_ex)(void *ptr) = NULL;


// extern zend_extension suhosin_zend_extension_entry;

/* {{{ suhosin_strcasestr */
static char *suhosin_strcasestr(char *haystack, char *needle)
{
	unsigned char *t, *h, *n;
	h = (unsigned char *) haystack;
conts:
	while (*h) {
		n = (unsigned char *) needle;
		if (toupper(*h++) == toupper(*n++)) {
			for (t=h; *n; t++, n++) {
				if (toupper(*t) != toupper(*n)) goto conts;
			}
			return ((char*)h-1);
		}
	}

	return (NULL);
}
/* }}} */


#define SUHOSIN_CODE_TYPE_UNKNOWN	0
#define SUHOSIN_CODE_TYPE_COMMANDLINE	1
#define SUHOSIN_CODE_TYPE_EVAL		2
#define SUHOSIN_CODE_TYPE_REGEXP	3
#define SUHOSIN_CODE_TYPE_ASSERT	4
#define SUHOSIN_CODE_TYPE_CFUNC		5
#define SUHOSIN_CODE_TYPE_SUHOSIN	6
#define SUHOSIN_CODE_TYPE_UPLOADED	7
#define SUHOSIN_CODE_TYPE_0FILE		8
#define SUHOSIN_CODE_TYPE_BLACKURL	9
#define SUHOSIN_CODE_TYPE_BADURL	10
#define SUHOSIN_CODE_TYPE_GOODFILE	11
#define SUHOSIN_CODE_TYPE_BADFILE	12
#define SUHOSIN_CODE_TYPE_LONGNAME	13
#define SUHOSIN_CODE_TYPE_MANYDOTS	14
#define SUHOSIN_CODE_TYPE_WRITABLE      15
#define SUHOSIN_CODE_TYPE_MBREGEXP	16

static int suhosin_check_filename(char *s, int len)
{
	char fname[MAXPATHLEN+1];
	char *t, *h, *h2, *index, *e;
	int tlen, i, count=0;
	uint indexlen;
	ulong numindex;
	zend_bool isOk;

	/* check if filename is too long */
	if (len > MAXPATHLEN) {
		return SUHOSIN_CODE_TYPE_LONGNAME;
	}
	memcpy(fname, s, len);
	fname[len] = 0; 
	s = (char *)&fname;
	e = s + len;

	/* check if ASCIIZ attack */
	if (len != strlen(s)) {
		return SUHOSIN_CODE_TYPE_0FILE;
	}
	
	/* disallow uploaded files */
	if (SG(rfc1867_uploaded_files)) {
		if (zend_hash_str_exists(SG(rfc1867_uploaded_files), (char *) s, e-s+1)) { // <--- TODO: range check
			return SUHOSIN_CODE_TYPE_UPLOADED;
		}
	}
		
	/* count number of directory traversals */
	for (i=0; i < len-3; i++) {
		if (s[i] == '.' && s[i+1] == '.' && (s[i+2] == '/' || s[i+2] == '\\')) {
			count++;
			i+=2;
		}
	}
	if (SUHOSIN7_G(executor_include_max_traversal) && SUHOSIN7_G(executor_include_max_traversal)<=count) {
		return SUHOSIN_CODE_TYPE_MANYDOTS;
	}
	
	SDEBUG("xxx %p %p",SUHOSIN7_G(include_whitelist),SUHOSIN7_G(include_blacklist));
	/* no black or whitelist then disallow all */
	if (SUHOSIN7_G(include_whitelist)==NULL && SUHOSIN7_G(include_blacklist)==NULL) {
		/* disallow all URLs */
		if (strstr(s, "://") != NULL || suhosin_strcasestr(s, "data:") != NULL) {
			return SUHOSIN_CODE_TYPE_BADURL;
		}
	} else 
	/* whitelist is stronger than blacklist */
	// if (SUHOSIN7_G(include_whitelist)) {
	// 	
	// 	do {
	// 		isOk = 0;
	// 		
	// 		h = strstr(s, "://");
	// 		h2 = suhosin_strcasestr(s, "data:");
	// 		h2 = h2 == NULL ? NULL : h2 + 4;
	// 		t = h = (h == NULL) ? h2 : ( (h2 == NULL) ? h : ( (h < h2) ? h : h2 ) );
	// 		if (h == NULL) break;
	// 						
	// 		while (t > s && (isalnum(t[-1]) || t[-1]=='_' || t[-1]=='.')) {
	// 			t--;
	// 		}
	// 		
	// 		tlen = e-t;
	// 		
	// 		zend_hash_internal_pointer_reset(SUHOSIN7_G(include_whitelist));
	// 		do {
	// 			int r = zend_hash_get_current_key_ex(SUHOSIN7_G(include_whitelist), &index, &indexlen, &numindex, 0, NULL);
	// 			
	// 			if (r==HASH_KEY_NON_EXISTANT) {
	// 				break;
	// 			}
	// 			if (r==HASH_KEY_IS_STRING) {
	// 				if (h-t <= indexlen-1 && tlen>=indexlen-1) {
	// 					if (strncasecmp(t, index, indexlen-1)==0) {
	// 						isOk = 1;
	// 						break;
	// 					}
	// 				}
	// 			}
	// 			
	// 			zend_hash_move_forward(SUHOSIN7_G(include_whitelist));
	// 		} while (1);
	// 		
	// 		/* not found in whitelist */
	// 		if (!isOk) {
	// 			return SUHOSIN_CODE_TYPE_BADURL;
	// 		}
	// 		
	// 		s = h + 1;
	// 	} while (1);
	// } else {
	// 	
	// 	do {
	// 		int tlen;
	// 		
	// 		h = strstr(s, "://");
	// 		h2 = suhosin_strcasestr(s, "data:");
	// 		h2 = h2 == NULL ? NULL : h2 + 4;
	// 		t = h = (h == NULL) ? h2 : ( (h2 == NULL) ? h : ( (h < h2) ? h : h2 ) );
	// 		if (h == NULL) break;
	// 						
	// 		while (t > s && (isalnum(t[-1]) || t[-1]=='_' || t[-1]=='.')) {
	// 			t--;
	// 		}
	// 
	// 		tlen = e-t;
	// 
	// 		zend_hash_internal_pointer_reset(SUHOSIN7_G(include_blacklist));
	// 		do {
	// 			int r = zend_hash_get_current_key_ex(SUHOSIN7_G(include_blacklist), &index, &indexlen, &numindex, 0, NULL);
	// 
	// 			if (r==HASH_KEY_NON_EXISTANT) {
	// 				break;
	// 			}
	// 			if (r==HASH_KEY_IS_STRING) {
	// 				if (h-t <= indexlen-1 && tlen>=indexlen-1) {
	// 					if (strncasecmp(t, index, indexlen-1)==0) {
	// 						return SUHOSIN_CODE_TYPE_BLACKURL;
	// 					}
	// 				}
	// 			}
	// 			
	// 			zend_hash_move_forward(SUHOSIN7_G(include_blacklist));
	// 		} while (1);
	// 		
	// 		s = h + 1;
	// 	} while (1);
	// }

	/* disallow writable files */
	if (!SUHOSIN7_G(executor_include_allow_writable_files)) {
		/* protection against *REMOTE* attacks, potential
		   race condition of access() is irrelevant */
		if (access(s, W_OK) == 0) {
			return SUHOSIN_CODE_TYPE_WRITABLE;
		}
	}

	return SUHOSIN_CODE_TYPE_GOODFILE;
}


// ZEND_API static int (*old_zend_stream_open)(const char *filename, zend_file_handle *handle);
// 
// static int suhosin_zend_stream_open(const char *filename, zend_file_handle *fh)
// {
// 	zend_execute_data *exd;
// 	exd=EG(current_execute_data);
// 	if (EG(in_execution) && (exd!=NULL) && (exd->opline != NULL) && (exd->opline->opcode == ZEND_INCLUDE_OR_EVAL)) {
// 		int filetype = suhosin_check_filename((char *)filename, strlen(filename));
// 		
// 		switch (filetype) {
// 		    case SUHOSIN_CODE_TYPE_LONGNAME:
// 			suhosin_log(S_INCLUDE, "Include filename ('%s') is too long", filename);
// 			suhosin_bailout();
// 			break;
// 
// 		    case SUHOSIN_CODE_TYPE_UPLOADED:
// 			suhosin_log(S_INCLUDE, "Include filename is an uploaded file");
// 			suhosin_bailout();
// 			break;
// 		    
// 		    case SUHOSIN_CODE_TYPE_0FILE:
// 			suhosin_log(S_INCLUDE, "Include filename contains an ASCIIZ character");
// 			suhosin_bailout();
// 			break;
// 		
// 		    case SUHOSIN_CODE_TYPE_WRITABLE:
// 			suhosin_log(S_INCLUDE, "Include filename ('%s') is writable by PHP process", filename);
// 			suhosin_bailout();
// 			break;		    	
// 
// 		    case SUHOSIN_CODE_TYPE_BLACKURL:
// 			suhosin_log(S_INCLUDE, "Include filename ('%s') is a URL that is forbidden by the blacklist", filename);
// 			suhosin_bailout();
// 			break;
// 			
// 		    case SUHOSIN_CODE_TYPE_BADURL:
// 			suhosin_log(S_INCLUDE, "Include filename ('%s') is a URL that is not allowed", filename);
// 			suhosin_bailout();
// 			break;
// 
// 		    case SUHOSIN_CODE_TYPE_MANYDOTS:
// 			suhosin_log(S_INCLUDE, "Include filename ('%s') contains too many '../'", filename);
// 			suhosin_bailout();
// 			break;
// 		}
// 	}
// 	return old_zend_stream_open(filename, fh);
// }


static int suhosin_detect_codetype(zend_op_array *op_array)
{
	if (op_array->filename == NULL) {
		return SUHOSIN_CODE_TYPE_UNKNOWN;
	}

	char *s = (char *)ZSTR_VAL(op_array->filename);

	/* eval, assert, create_function, preg_replace  */
	if (op_array->type == ZEND_EVAL_CODE) {
	
		if (s == NULL) {
			return SUHOSIN_CODE_TYPE_UNKNOWN;
		}
	
		if (strstr(s, "eval()'d code") != NULL) {
			return SUHOSIN_CODE_TYPE_EVAL;
		}

		if (strstr(s, "regexp code") != NULL) {
			return SUHOSIN_CODE_TYPE_REGEXP;
		}

		if (strstr(s, "mbregex replace") != NULL) {
			return SUHOSIN_CODE_TYPE_MBREGEXP;
		}

		if (strstr(s, "assert code") != NULL) {
			return SUHOSIN_CODE_TYPE_ASSERT;
		}

		if (strstr(s, "runtime-created function") != NULL) {
			return SUHOSIN_CODE_TYPE_CFUNC;
		}
		
		if (strstr(s, "Command line code") != NULL) {
			return SUHOSIN_CODE_TYPE_COMMANDLINE;
		}

		if (strstr(s, "Command line begin code") != NULL) {
			return SUHOSIN_CODE_TYPE_COMMANDLINE;
		}

		if (strstr(s, "Command line run code") != NULL) {
			return SUHOSIN_CODE_TYPE_COMMANDLINE;
		}

		if (strstr(s, "Command line end code") != NULL) {
			return SUHOSIN_CODE_TYPE_COMMANDLINE;
		}
		
		if (strstr(s, "suhosin internal code") != NULL) {
			return SUHOSIN_CODE_TYPE_SUHOSIN;
		}
		
	} else {

		return suhosin_check_filename(s, strlen(s));

	}
	
	return SUHOSIN_CODE_TYPE_UNKNOWN;
}

/* {{{ void suhosin_execute_ex(zend_op_array *op_array)
 *    This function provides a hook for execution */
ZEND_API static void suhosin_execute_ex(zend_execute_data *execute_data)
{
	// SDEBUG("X------------------------->")
	// TODO: check execute_data + execute_data->func
	
	zend_op_array *new_op_array;
	int op_array_type;//, len;
	// char *fn;
	zval cs;
	zend_ulong orig_code_type;
	unsigned long *suhosin_flags = NULL;
	
	/* log variable dropping statistics */
	if (SUHOSIN7_G(abort_request)) {
		
		SUHOSIN7_G(abort_request) = 0; /* we only want this to happen the first time */
		
		if (SUHOSIN7_G(att_request_variables)-SUHOSIN7_G(cur_request_variables) > 0) {
			suhosin_log(S_VARS, "dropped %u request variables - (%u in GET, %u in POST, %u in COOKIE)",
			SUHOSIN7_G(att_request_variables)-SUHOSIN7_G(cur_request_variables),
			SUHOSIN7_G(att_get_vars)-SUHOSIN7_G(cur_get_vars),
			SUHOSIN7_G(att_post_vars)-SUHOSIN7_G(cur_post_vars),
			SUHOSIN7_G(att_cookie_vars)-SUHOSIN7_G(cur_cookie_vars));
		}
	
		// if (!SUHOSIN7_G(simulation) && SUHOSIN7_G(filter_action)) {
		// 
		// 	char *action = SUHOSIN7_G(filter_action);
		// 	long code = -1;
		// 		
		// 	while (*action == ' ' || *action == '\t') action++;
		// 
		// 	if (*action >= '0' && *action <= '9') {
		// 		char *end = action;
		// 		while (*end && *end != ',' && *end != ';') end++;
		// 		code = zend_atoi(action, end-action);
		// 		action = end;
		// 	}
		// 
		// 	while (*action == ' ' || *action == '\t' || *action == ',' || *action == ';') action++;
		// 
		// 	if (*action) {
		// 	
		// 		if (strncasecmp("http://", action, sizeof("http://")-1)==0
		// 		|| strncasecmp("https://", action, sizeof("https://")-1)==0) {
		// 			sapi_header_line ctr = {0};
		// 		
		// 			if (code == -1) {
		// 				code = 302;
		// 			}
		// 		
		// 			ctr.line_len = spprintf(&ctr.line, 0, "Location: %s", action);
		// 			ctr.response_code = code;
		// 			sapi_header_op(SAPI_HEADER_REPLACE, &ctr);
		// 			efree(ctr.line);
		// 		} else {
		// 			zend_file_handle file_handle;
		// 			zend_op_array *new_op_array;
		// 			zval *result = NULL;
		// 		
		// 			if (code == -1) {
		// 				code = 200;
		// 			}
		// 		
		// 			if (zend_stream_open(action, &file_handle) == SUCCESS) {
		// 				if (!file_handle.opened_path) {
		// 					file_handle.opened_path = estrndup(action, strlen(action));
		// 				}
		// 				new_op_array = zend_compile_file(&file_handle, ZEND_REQUIRE);
		// 				zend_destroy_file_handle(&file_handle);
		// 				if (new_op_array) {
		// 					EG(return_value_ptr_ptr) = &result;
		// 					EG(active_op_array) = new_op_array;
		// 					zend_execute(new_op_array);
		// 					destroy_op_array(new_op_array);
		// 					efree(new_op_array);
		// 
		// 					if (!EG(exception))
		// 					{
		// 						if (EG(return_value_ptr_ptr)) {
		// 							zval_ptr_dtor(EG(return_value_ptr_ptr));
		// 							EG(return_value_ptr_ptr) = NULL;
		// 						}
		// 					}
		// 				} else {
		// 					code = 500;
		// 				}
		// 			} else {
		// 				code = 500;
		// 			}
		// 		}
		// 	}
		// 
		// 	sapi_header_op(SAPI_HEADER_SET_STATUS, (void *)code);
		// 	zend_bailout();
		// }
	}
	
	// SDEBUG("%s %s", op_array->filename, op_array->function_name);
	
	SUHOSIN7_G(execution_depth)++;
	
	if (SUHOSIN7_G(max_execution_depth) && SUHOSIN7_G(execution_depth) > SUHOSIN7_G(max_execution_depth)) {
		suhosin_log(S_EXECUTOR|S_GETCALLER, "maximum execution depth reached - script terminated");
		suhosin_bailout();
	}
	
	// fn = (char *)execute_data->func->op_array.filename;
	// len = strlen(fn);
	
	orig_code_type = SUHOSIN7_G(in_code_type);
	if (execute_data->func->op_array.type == ZEND_EVAL_CODE) {
		SUHOSIN7_G(in_code_type) = SUHOSIN_EVAL;
	} else {
		// if (suhosin_zend_extension_entry.resource_number != -1) {
		// 	suhosin_flags = (unsigned long *) &execute_data->func->op_array.reserved[suhosin_zend_extension_entry.resource_number];
		// 	SDEBUG("suhosin flags: %08lx", *suhosin_flags);
		// 	
		// 	if (*suhosin_flags & SUHOSIN_FLAG_CREATED_BY_EVAL) {
		// 		SUHOSIN7_G(in_code_type) = SUHOSIN_EVAL;
		// 	}
		// 	if (*suhosin_flags & SUHOSIN_FLAG_NOT_EVALED_CODE) {
		// 		goto not_evaled_code;
		// 	}
		// }
		
		if (zend_string_equals_literal(execute_data->func->op_array.filename, "eval()'d code")) {
			SUHOSIN7_G(in_code_type) = SUHOSIN_EVAL;
		} // else {
		// 	if (suhosin_flags) {
		// 		*suhosin_flags |= SUHOSIN_FLAG_NOT_EVALED_CODE;
		// 	}
		// }
	}
not_evaled_code:
	SDEBUG("code type %llu", SUHOSIN7_G(in_code_type));
	if (execute_data->func->op_array.function_name) {
		goto continue_execution;
	}

/*	if (SUHOSIN7_G(deactivate)) {
		goto continue_execution;
	}
*/	

	op_array_type = suhosin_detect_codetype(&execute_data->func->op_array);
	
	switch (op_array_type) {
	    case SUHOSIN_CODE_TYPE_EVAL:
		    if (SUHOSIN7_G(executor_disable_eval)) {
			    suhosin_log(S_EXECUTOR|S_GETCALLER, "use of eval is forbidden by configuration");
			    if (!SUHOSIN7_G(simulation)) {
				    zend_error(E_ERROR, "SUHOSIN - Use of eval is forbidden by configuration");
			    }
		    }
		    break;
		    
	    case SUHOSIN_CODE_TYPE_REGEXP:
		    if (SUHOSIN7_G(executor_disable_emod)) {
			    suhosin_log(S_EXECUTOR|S_GETCALLER, "use of preg_replace() with /e modifier is forbidden by configuration");
			    if (!SUHOSIN7_G(simulation)) {
				    zend_error(E_ERROR, "SUHOSIN - Use of preg_replace() with /e modifier is forbidden by configuration");
			    }
		    }
		    break;
		    
		case SUHOSIN_CODE_TYPE_MBREGEXP:
			/* XXX TODO: Do we want to disallow this, too? */
			break;
		
		case SUHOSIN_CODE_TYPE_ASSERT:
			break;
		
		case SUHOSIN_CODE_TYPE_CFUNC:
			break;
		
		case SUHOSIN_CODE_TYPE_LONGNAME:
			suhosin_log(S_INCLUDE|S_GETCALLER, "Include filename ('%s') is too long", ZSTR_VAL(execute_data->func->op_array.filename));
			suhosin_bailout();
			break;

		case SUHOSIN_CODE_TYPE_MANYDOTS:
			suhosin_log(S_INCLUDE|S_GETCALLER, "Include filename ('%s') contains too many '../'", ZSTR_VAL(execute_data->func->op_array.filename));
			suhosin_bailout();
			break;
	    
		case SUHOSIN_CODE_TYPE_UPLOADED:
			suhosin_log(S_INCLUDE|S_GETCALLER, "Include filename is an uploaded file");
			suhosin_bailout();
			break;
			
	    case SUHOSIN_CODE_TYPE_0FILE:
			suhosin_log(S_INCLUDE|S_GETCALLER, "Include filename contains an ASCIIZ character");
			suhosin_bailout();
			break;
			
		case SUHOSIN_CODE_TYPE_WRITABLE:
			suhosin_log(S_INCLUDE|S_GETCALLER, "Include filename ('%s') is writable by PHP process", ZSTR_VAL(execute_data->func->op_array.filename));
			suhosin_bailout();
			break;		    	

	    case SUHOSIN_CODE_TYPE_BLACKURL:
			suhosin_log(S_INCLUDE|S_GETCALLER, "Include filename ('%s') is a URL that is forbidden by the blacklist", ZSTR_VAL(execute_data->func->op_array.filename));
			suhosin_bailout();
			break;
			
	    case SUHOSIN_CODE_TYPE_BADURL:
			suhosin_log(S_INCLUDE|S_GETCALLER, "Include filename ('%s') is a URL that is not allowed", ZSTR_VAL(execute_data->func->op_array.filename));
			suhosin_bailout();
			break;

	    case SUHOSIN_CODE_TYPE_BADFILE:
// 		    cs.type = IS_STRING;
// #define DIE_WITH_MSG "die('disallowed_file'.chr(10).chr(10));"
// 		    cs.value.str.val = estrndup(DIE_WITH_MSG, sizeof(DIE_WITH_MSG)-1);
// 		    cs.value.str.len = sizeof(DIE_WITH_MSG)-1;
// 		    new_op_array = compile_string(&cs, "suhosin internal code");
// 		    if (new_op_array) {
// 				op_array = new_op_array;
// 				goto continue_execution;
// 		    }
		    suhosin_bailout();
		    break;

	    case SUHOSIN_CODE_TYPE_COMMANDLINE:
	    case SUHOSIN_CODE_TYPE_SUHOSIN:
	    case SUHOSIN_CODE_TYPE_UNKNOWN:
	    case SUHOSIN_CODE_TYPE_GOODFILE:
			goto continue_execution;
	}

continue_execution:
	old_execute_ex (execute_data);

	/* nothing to do */
	SUHOSIN7_G(in_code_type) = orig_code_type;
	SUHOSIN7_G(execution_depth)--;
}
/* }}} */


#define IH_HANDLER_PARAMS_REST int ht, zval *return_value
#define IH_HANDLER_PARAMS internal_function_handler *ih, IH_HANDLER_PARAMS_REST
#define IH_HANDLER_PARAM_PASSTHRU ih, ht, return_value


HashTable ihandler_table;

typedef struct _internal_function_handler {

	char *name;
	int (*handler)(struct _internal_function_handler *ih, IH_HANDLER_PARAMS_REST);
	void *arg1;
	void *arg2;
	void *arg3;

} internal_function_handler;

// int ih_preg_replace(IH_HANDLER_PARAMS)
// {
// 	zval **regex,
// 	     **replace,
// 	     **subject,
// 	     **limit, **zcount;
// 
// 	 if (zend_parse_parameters(ZEND_NUM_ARGS(), "ZZZ|ZZ", &regex, &replace, &subject, &limit, &zcount) == FAILURE) {
// 	 	return(0);
// 	 }
// 		
// 	if (Z_TYPE_PP(regex) == IS_ARRAY) {
// 		zval	**regex_entry;
// 		
// 		zend_hash_internal_pointer_reset(Z_ARRVAL_PP(regex));
// 		/* For each entry in the regex array, get the entry */
// 		while (zend_hash_get_current_data(Z_ARRVAL_PP(regex), (void **)&regex_entry) == SUCCESS) {
// 			
// 			if (Z_TYPE_PP(regex_entry) == IS_STRING) {
// 				if (strlen(Z_STRVAL_PP(regex_entry)) != Z_STRLEN_PP(regex_entry)) {
// 					suhosin_log(S_EXECUTOR, "string termination attack on first preg_replace parameter detected");
// 	    				if (!SUHOSIN7_G(simulation)) {
// 						RETVAL_FALSE;
// 						return (1);
// 					}
// 				}
// 			}
// 				
// 			zend_hash_move_forward(Z_ARRVAL_PP(regex));
// 			
// 		}
// 			
// 	} else if (Z_TYPE_PP(regex) == IS_STRING) {
// 		if (strlen(Z_STRVAL_PP(regex)) != Z_STRLEN_PP(regex)) {
// 			suhosin_log(S_EXECUTOR, "string termination attack on first preg_replace parameter detected");
// 			if (!SUHOSIN7_G(simulation)) {
// 				RETVAL_FALSE;
// 				return (1);
// 			}
// 		}
// 	}
// 	
// 	return (0);
// }

// int ih_symlink(IH_HANDLER_PARAMS)
// {
// 	if (SUHOSIN7_G(executor_allow_symlink)) {
// 		return (0);
// 	}
// 	
// 	if (PG(open_basedir) && PG(open_basedir)[0]) {
// 		suhosin_log(S_EXECUTOR, "symlink called during open_basedir");
// 		if (!SUHOSIN7_G(simulation)) {
// 			RETVAL_FALSE;
// 			return (1);
// 		}
// 	}
// 	
// 	return (0);
// }

// int ih_mail(IH_HANDLER_PARAMS)
// {
// 	char *to=NULL, *message=NULL, *headers=NULL;
// 	char *subject=NULL, *extra_cmd=NULL;
// 	char *tmp;
// 	int to_len, message_len, headers_len;
// 	int subject_len, extra_cmd_len;
// 
// 	if (SUHOSIN7_G(mailprotect) == 0) {
// 		return (0);
// 	}
// 
// 	if (zend_parse_parameters(ZEND_NUM_ARGS(), "sss|ss",
// 						  &to, &to_len,
// 						  &subject, &subject_len,
// 						  &message, &message_len,
// 						  &headers, &headers_len,
// 						  &extra_cmd, &extra_cmd_len
// 						  ) == FAILURE) {
// 		RETVAL_FALSE;
// 		return (1);
// 	}
// 
// 	if (headers_len > 0 && headers &&
// 		(strstr(headers, "\n\n") || strstr(headers, "\n\r\n") /* double newline */
// 			|| *headers == '\n' || (headers[0] == '\r' && headers[1] == '\n') /* starts with newline */
// 	)) {
// 		suhosin_log(S_MAIL, "mail() - double newline in headers, possible injection, mail dropped");
// 		if (!SUHOSIN7_G(simulation)) {
// 			RETVAL_FALSE;
// 			return (1);
// 		}
// 	}
// 
// 	/* check for spam attempts with buggy webforms */
// 	if (to_len > 0 && to) {
// 		do {
// 			if ((tmp = strchr(to, '\n')) == NULL)
// 				tmp = strchr(to, '\r');
// 			if (tmp == NULL) break;
// 			to = tmp + 1;
// 			if (!isspace(*to)) break;
// 		} while (1);
// 		if (tmp != NULL) {
// 			suhosin_log(S_MAIL, "mail() - newline in To header, possible injection, mail dropped");
// 			if (!SUHOSIN7_G(simulation)) {
// 				RETVAL_FALSE;
// 				return (1);
// 			}
// 		}
// 	}
// 
// 	if (subject_len > 0 && subject) {
// 		do {
// 			if ((tmp = strchr(subject, '\n')) == NULL)
// 				tmp = strchr(subject, '\r');
// 			if (tmp == NULL) break;
// 			subject = tmp + 1;
// 			if (!isspace(*subject)) break;
// 		} while (1);
// 		if (tmp != NULL) {
// 			suhosin_log(S_MAIL, "mail() - newline in Subject header, possible injection, mail dropped");
// 			if (!SUHOSIN7_G(simulation)) {
// 				RETVAL_FALSE;
// 				return (1);
// 			}
// 		}
// 	}
// 		
// 	if (SUHOSIN7_G(mailprotect) > 1) {
// 		/* search for to, cc or bcc headers */
// 		if (headers_len > 0 && headers != NULL) {
// 			if (strncasecmp(headers, "to:", sizeof("to:") - 1) == 0 || suhosin_strcasestr(headers, "\nto:")) {
// 				suhosin_log(S_MAIL, "mail() - To: headers aren't allowed in the headers parameter.");
// 				if (!SUHOSIN7_G(simulation)) {
// 					RETVAL_FALSE;
// 					return (1);
// 				}
// 			}
// 			
// 			if (strncasecmp(headers, "cc:", sizeof("cc:") - 1) == 0 || suhosin_strcasestr(headers, "\ncc:")) {
// 				suhosin_log(S_MAIL, "mail() - CC: headers aren't allowed in the headers parameter.");
// 				if (!SUHOSIN7_G(simulation)) {
// 					RETVAL_FALSE;
// 					return (1);
// 				}
// 			}
// 
// 			if (strncasecmp(headers, "bcc:", sizeof("bcc:") - 1) == 0 || suhosin_strcasestr(headers, "\nbcc:")) {
// 				suhosin_log(S_MAIL, "mail() - BCC: headers aren't allowed in the headers parameter.");
// 				if (!SUHOSIN7_G(simulation)) {
// 					RETVAL_FALSE;
// 					return (1);
// 				}
// 			}
// 		}
// 	}
// 
// 	return (0);
// }

// #define SQLSTATE_SQL        0
// #define SQLSTATE_IDENTIFIER 1
// #define SQLSTATE_STRING     2
// #define SQLSTATE_COMMENT    3
// #define SQLSTATE_MLCOMMENT  4
// 
// int ih_querycheck(IH_HANDLER_PARAMS)
// {
// 	void **p = zend_vm_stack_top() - 1;
// 	unsigned long arg_count;
// 	zval **arg;
// 	char *query, *s, *e;
// 	zval *backup;
// 	int len;
// 	char quote;
// 	int state = SQLSTATE_SQL;
// 	int cnt_union = 0, cnt_select = 0, cnt_comment = 0, cnt_opencomment = 0;
// 	int mysql_extension = 0;
// 
// 	
// 	SDEBUG("function: %s", ih->name);
// 	arg_count = (unsigned long) *p;
// 
// 	if (ht < (long) ih->arg1) {
// 		return (0);
// 	}
//     
// 	if ((long) ih->arg2) {
//     	    mysql_extension = 1;
// 	}
// 	
// 	arg = (zval **) p - (arg_count - (long) ih->arg1 + 1); /* count from 0 */
// 
// 	backup = *arg;
// 	if (Z_TYPE_P(backup) != IS_STRING) {
// 		return (0);
// 	}
// 	len = Z_STRLEN_P(backup);
// 	query = Z_STRVAL_P(backup);
// 	SDEBUG("SQL |%s|", query);
// 	
// 	s = query;
// 	e = s+len;
// 	
// 	while (s < e) {
// 	    switch (state)
// 	    {
//     		case SQLSTATE_SQL:
//     		    switch (s[0])
//     		    {
//         		case '`':
//         		    state = SQLSTATE_IDENTIFIER;
//         		    quote = '`';
//         		    break;
//         		case '\'':
//         		case '"':
//         		    state = SQLSTATE_STRING;
//         		    quote = *s;
//         		    break;
//         		case '/':
//         		    if (s[1]=='*') {
//                         if (mysql_extension == 1 && s[2] == '!') {
//                             s += 2;
//                             break;
//                         }
//             			s++;
//             			state = SQLSTATE_MLCOMMENT;
//         			    cnt_comment++;
//         		    }
//         		    break;
//     			case '-':
//         		    if (s[1]=='-') {
//         			s++;
//         			state = SQLSTATE_COMMENT;
//         			cnt_comment++;
//         		    }
//         		    break;
//     			case '#':
//         		    state = SQLSTATE_COMMENT;
//         		    cnt_comment++;
//         		    break;
//         		case 'u':
//     			case 'U':
//         		    if (strncasecmp("union", s, 5)==0) {
//             			s += 4;
//         			cnt_union++;
//         		    }
//         		    break;
//     			case 's':
//     			case 'S':
//         		    if (strncasecmp("select", s, 6)==0) {
//             			s += 5;
//         			cnt_select++;
//         		    }
//         		    break;
//     		    }
//     		    break;
//     		case SQLSTATE_STRING:
// 		case SQLSTATE_IDENTIFIER:
//     		    if (s[0] == quote) {
//         		if (s[1] == quote) {
//         		    s++;
//     			} else {
//         		    state = SQLSTATE_SQL;
//     			}
//     		    }
//     		    if (s[0] == '\\') {
//     			s++;
//     		    }
//     		    break;
// 		case SQLSTATE_COMMENT:
//     		    while (s[0] && s[0] != '\n') {
//     			s++;        
//     		    }
//     		    state = SQLSTATE_SQL;
//     		    break;
//     		case SQLSTATE_MLCOMMENT:
//     		    while (s[0] && (s[0] != '*' || s[1] != '/')) {
//     			s++;
//     		    }
//     		    if (s[0]) {
//     			state = SQLSTATE_SQL;
//     		    }
//     		    break;
// 	    }
// 	    s++;
// 	}
// 	if (state == SQLSTATE_MLCOMMENT) {
// 	    cnt_opencomment = 1;
// 	}
// 	
// 	if (cnt_opencomment && SUHOSIN7_G(sql_opencomment)>0) {
// 	    suhosin_log(S_SQL, "Open comment in SQL query: '%*s'", len, query);
// 	    if (SUHOSIN7_G(sql_opencomment)>1) {
// 		suhosin_bailout();
// 	    }
// 	}
// 	
// 	if (cnt_comment && SUHOSIN7_G(sql_comment)>0) {
// 	    suhosin_log(S_SQL, "Comment in SQL query: '%*s'", len, query);
// 	    if (SUHOSIN7_G(sql_comment)>1) {
// 		suhosin_bailout();
// 	    }
// 	}
// 
// 	if (cnt_union && SUHOSIN7_G(sql_union)>0) {
// 	    suhosin_log(S_SQL, "UNION in SQL query: '%*s'", len, query);
// 	    if (SUHOSIN7_G(sql_union)>1) {
// 		suhosin_bailout();
// 	    }
// 	}
// 
// 	if (cnt_select>1 && SUHOSIN7_G(sql_mselect)>0) {
// 	    suhosin_log(S_SQL, "Multiple SELECT in SQL query: '%*s'", len, query);
// 	    if (SUHOSIN7_G(sql_mselect)>1) {
// 		suhosin_bailout();
// 	    }
// 	}
//     
// 	return (0);
// }
// 
// 
// int ih_fixusername(IH_HANDLER_PARAMS)
// {
// 	void **p = zend_vm_stack_top() - 1;
// 	unsigned long arg_count;
// 	zval **arg;
// 	char *prefix, *postfix, *user, *user_match, *cp;
// 	zval *backup, *my_user;
// 	int prefix_len, postfix_len, len;
// 	
// 	SDEBUG("function (fixusername): %s", ih->name);
// 	
// 	prefix = SUHOSIN7_G(sql_user_prefix);
// 	postfix = SUHOSIN7_G(sql_user_postfix);
// 	user_match = SUHOSIN7_G(sql_user_match);
// 	
// 	arg_count = (unsigned long) *p;
// 
// 	if (ht < (long) ih->arg1) {
// 		return (0);
// 	}
// 	
// 	arg = (zval **) p - (arg_count - (long) ih->arg1 + 1); /* count from 0 */
// 
// 	backup = *arg;
// 	if (Z_TYPE_P(backup) != IS_STRING) {
// 		user = "";
// 		len = 0;
// 	} else {
// 		len = Z_STRLEN_P(backup);
// 		user = Z_STRVAL_P(backup);
// 	}
// 
// 	cp = user;
// 	while (cp < user+len) {
// 		if (*cp < 32) {
// 			suhosin_log(S_SQL, "SQL username contains invalid characters");
// 			if (!SUHOSIN7_G(simulation)) {
// 				RETVAL_FALSE;
// 				return (1);
// 			}
// 			break;
// 		}
// 		cp++;
// 	}
// 
// 	if ((prefix != NULL && prefix[0]) || (postfix != NULL && postfix[0])) {
// 		if (prefix == NULL) {
// 			prefix = "";
// 		}
// 		if (postfix == NULL) {
// 			postfix = "";
// 		}
// 		prefix_len = strlen(prefix);
// 		postfix_len = strlen(postfix);
// 		
// 		MAKE_STD_ZVAL(my_user);
// 		my_user->type = IS_STRING;
// 		my_user->value.str.len = spprintf(&my_user->value.str.val, 0, "%s%s%s", prefix, user, postfix);
// 	
// 		/* XXX: memory_leak? */
// 		*arg = my_user;	
// 		
// 		len = Z_STRLEN_P(my_user);
// 		user = Z_STRVAL_P(my_user);
// 	}
// 	
// 	if (user_match && user_match[0]) {
// #ifdef HAVE_FNMATCH
// 		if (fnmatch(user_match, user, 0) != 0) {
// 			suhosin_log(S_SQL, "SQL username ('%s') does not match suhosin.sql.user_match ('%s')", user, user_match);
// 			if (!SUHOSIN7_G(simulation)) {
// 				RETVAL_FALSE;
// 				return (1);
// 			}
// 		}
// #else
// #warning no support for fnmatch() - setting suhosin.sql.user_match will always fail.
// 		suhosin_log(S_SQL, "suhosin.sql.user_match specified, but system does not support fnmatch()");
// 		if (!SUHOSIN7_G(simulation)) {
// 			RETVAL_FALSE;
// 			return (1);
// 		}
// #endif
// 	}
// 	
// 	SDEBUG("function: %s - user: %s", ih->name, user);
// 
// 	return (0);
// }
// 
// 
// static int ih_function_exists(IH_HANDLER_PARAMS)
// {
// 	zval **function_name;
// 	zend_function *func;
// 	char *lcname;
// 	zend_bool retval;
// 	int func_name_len;
// 	
// 	if (ZEND_NUM_ARGS()!=1 || zend_get_parameters_ex(1, &function_name)==FAILURE) {
// 		ZEND_WRONG_PARAM_COUNT_WITH_RETVAL(1);
// 	}
// 	convert_to_string_ex(function_name);
// 	func_name_len = Z_STRLEN_PP(function_name);
// 	lcname = estrndup(Z_STRVAL_PP(function_name), func_name_len);	
// 	zend_str_tolower(lcname, func_name_len);
// 
// 	retval = (zend_hash_find(EG(function_table), lcname, func_name_len+1, (void **)&func) == SUCCESS);
// 	
// 	/*
// 	 * A bit of a hack, but not a bad one: we see if the handler of the function
// 	 * is actually one that displays "function is disabled" message.
// 	 */
// 	if (retval && func->type == ZEND_INTERNAL_FUNCTION &&
// 		func->internal_function.handler == zif_display_disabled_function) {
// 		retval = 0;
// 	}
// 
// 	/* Now check if function is forbidden by Suhosin */
// 	if (SUHOSIN7_G(in_code_type) == SUHOSIN_EVAL) {
// 		if (SUHOSIN7_G(eval_whitelist) != NULL) {
// 			if (!zend_hash_exists(SUHOSIN7_G(eval_whitelist), lcname, func_name_len+1)) {
// 			    retval = 0;
// 			}
// 		} else if (SUHOSIN7_G(eval_blacklist) != NULL) {
// 			if (zend_hash_exists(SUHOSIN7_G(eval_blacklist), lcname, func_name_len+1)) {
// 			    retval = 0;
// 			}
// 		}
// 	}
// 	
// 	if (SUHOSIN7_G(func_whitelist) != NULL) {
// 		if (!zend_hash_exists(SUHOSIN7_G(func_whitelist), lcname, func_name_len+1)) {
// 		    retval = 0;
// 		}
// 	} else if (SUHOSIN7_G(func_blacklist) != NULL) {
// 		if (zend_hash_exists(SUHOSIN7_G(func_blacklist), lcname, func_name_len+1)) {
// 		    retval = 0;
// 		}
// 	}
// 
// 	efree(lcname);
// 
// 	RETVAL_BOOL(retval);
// 	return (1);
// }

// #include "execute_rnd.inc.c"

internal_function_handler ihandlers[] = {
	// { "preg_replace", ih_preg_replace, NULL, NULL, NULL },
	// { "mail", ih_mail, NULL, NULL, NULL },
	// { "symlink", ih_symlink, NULL, NULL, NULL },
	
	// { "srand", ih_srand, NULL, NULL, NULL },
	// { "mt_srand", ih_mt_srand, NULL, NULL, NULL },
	// { "rand", ih_rand, NULL, NULL, NULL },
	// { "mt_rand", ih_mt_rand, NULL, NULL, NULL },
	// { "getrandmax", ih_getrandmax, NULL, NULL, NULL },
	// { "mt_getrandmax", ih_getrandmax, NULL, NULL, NULL },
	
	// { "function_exists", ih_function_exists, NULL, NULL, NULL },
	
	/* Mysqli */
	// { "mysqli::mysqli", ih_fixusername, (void *)2, NULL, NULL },
	// { "mysqli_connect", ih_fixusername, (void *)2, NULL, NULL },
	// { "mysqli::real_connect", ih_fixusername, (void *)2, NULL, NULL },
	// { "mysqli_real_connect", ih_fixusername, (void *)3, NULL, NULL },
	// { "mysqli_change_user", ih_fixusername, (void *)2, NULL, NULL },
	// { "mysqli::change_user", ih_fixusername, (void *)1, NULL, NULL },
	
	// { "mysqli::query", ih_querycheck, (void *)1, (void *)1, NULL },
	// { "mysqli_query", ih_querycheck, (void *)2, (void *)1, NULL },
	// { "mysqli::multi_query", ih_querycheck, (void *)1, (void *)1, NULL },
	// { "mysqli_multi_query", ih_querycheck, (void *)2, (void *)1, NULL },
	// { "mysqli::prepare", ih_querycheck, (void *)1, (void *)1, NULL },
	// { "mysqli_prepare", ih_querycheck, (void *)2, (void *)1, NULL },
	// { "mysqli::real_query", ih_querycheck, (void *)1, (void *)1, NULL },
	// { "mysqli_real_query", ih_querycheck, (void *)2, (void *)1, NULL },
	// { "mysqli::send_query", ih_querycheck, (void *)1, (void *)1, NULL },
	// { "mysqli_send_query", ih_querycheck, (void *)2, (void *)1, NULL },
	// // removed in PHP 5.3
	// { "mysqli_master_query", ih_querycheck, (void *)2, (void *)1, NULL },
	// { "mysqli_slave_query", ih_querycheck, (void *)2, (void *)1, NULL },
	// ----
	
	/* Mysql API - deprecated in PHP 5.5 */
	// { "mysql_connect", ih_fixusername, (void *)2, NULL, NULL },
	// { "mysql_pconnect", ih_fixusername, (void *)2, NULL, NULL },
	// { "mysql_query", ih_querycheck, (void *)1, (void *)1, NULL },
	// { "mysql_db_query", ih_querycheck, (void *)2, (void *)1, NULL },
	// { "mysql_unbuffered_query", ih_querycheck, (void *)1, (void *)1, NULL },
	
#ifdef SUHOSIN_EXPERIMENTAL
	/* MaxDB */
	// { "maxdb::maxdb", ih_fixusername, (void *)2, NULL, NULL },
	// { "maxdb_connect", ih_fixusername, (void *)2, NULL, NULL },
	// { "maxdb::real_connect", ih_fixusername, (void *)2, NULL, NULL },
	// { "maxdb_real_connect", ih_fixusername, (void *)3, NULL, NULL },
	// { "maxdb::change_user", ih_fixusername, (void *)1, NULL, NULL },
	// { "maxdb_change_user", ih_fixusername, (void *)2, NULL, NULL },
	// 
	// { "maxdb_master_query", ih_querycheck, (void *)2, NULL, NULL },
	// { "maxdb::multi_query", ih_querycheck, (void *)1, NULL, NULL },
	// { "maxdb_multi_query", ih_querycheck, (void *)2, NULL, NULL },
	// { "maxdb::query", ih_querycheck, (void *)1, NULL, NULL },
	// { "maxdb_query", ih_querycheck, (void *)2, NULL, NULL },
	// { "maxdb::real_query", ih_querycheck, (void *)1, NULL, NULL },
	// { "maxdb_real_query", ih_querycheck, (void *)2, NULL, NULL },
	// { "maxdb::send_query", ih_querycheck, (void *)1, NULL, NULL },
	// { "maxdb_send_query", ih_querycheck, (void *)2, NULL, NULL },
	// { "maxdb::prepare", ih_querycheck, (void *)1, NULL, NULL },
	// { "maxdb_prepare", ih_querycheck, (void *)2, NULL, NULL },

	/* PDO */
		/* note: mysql conditional comments not supported here */
	// { "pdo::__construct", ih_fixusername, (void *)2, NULL, NULL }, /* note: username may come from dsn (param 1) */
	// { "pdo::query", ih_querycheck, (void *)1, NULL, NULL },
	// { "pdo::prepare", ih_querycheck, (void *)1, NULL, NULL },
	// { "pdo::exec", ih_querycheck, (void *)1, NULL, NULL },
	
	/* Oracle OCI8 */
	// { "ocilogon", ih_fixusername, (void *)1, NULL, NULL },
	// { "ociplogon", ih_fixusername, (void *)1, NULL, NULL },
	// { "ocinlogon", ih_fixusername, (void *)1, NULL, NULL },
	// { "oci_connect", ih_fixusername, (void *)1, NULL, NULL },
	// { "oci_pconnect", ih_fixusername, (void *)1, NULL, NULL },
	// { "oci_new_connect", ih_fixusername, (void *)1, NULL, NULL },

	/* FrontBase */
	// { "fbsql_connect", ih_fixusername, (void *)2, NULL, NULL },
	// { "fbsql_pconnect", ih_fixusername, (void *)2, NULL, NULL },
	// { "fbsql_change_user", ih_fixusername, (void *)1, NULL, NULL },
	// { "fbsql_username", ih_fixusername, (void *)2, NULL, NULL },

	/* Informix */
	// { "ifx_connect", ih_fixusername, (void *)2, NULL, NULL },
	// { "ifx_pconnect", ih_fixusername, (void *)2, NULL, NULL },
	// 
	/* Firebird/InterBase */
	// { "ibase_connect", ih_fixusername, (void *)2, NULL, NULL },
	// { "ibase_pconnect", ih_fixusername, (void *)2, NULL, NULL },
	// { "ibase_service_attach", ih_fixusername, (void *)2, NULL, NULL },

	/* Microsoft SQL Server */
	// { "mssql_connect", ih_fixusername, (void *)2, NULL, NULL },
	// { "mssql_pconnect", ih_fixusername, (void *)2, NULL, NULL },
#endif

	{ NULL, NULL, NULL, NULL, NULL }
};

#define FUNCTION_WARNING() zend_error(E_WARNING, "%s() has been disabled for security reasons", suhosin_get_active_function_name());
#define FUNCTION_SIMULATE_WARNING() zend_error(E_WARNING, "SIMULATION - %s() has been disabled for security reasons", suhosin_get_active_function_name());

/* {{{ void suhosin_execute_internal
 *    This function provides a hook for internal execution */

#define EX_T(offset) (*EX_TMP_VAR(execute_data_ptr, offset))

ZEND_API static void suhosin_execute_internal(zend_execute_data *execute_data, zval *return_value)
{
	// zval *return_value;
	// zval **return_value_ptr;
	// zval *this_ptr;
	int ht = 0;
	
	// if (fci) {
	// 	return_value = *fci->retval_ptr_ptr;
	// 	return_value_ptr = fci->retval_ptr_ptr;
	// 	this_ptr = fci->object_ptr;
	// 	ht = fci->param_count;
	// } else {
	// 	temp_variable *ret = &EX_T(execute_data_ptr->opline->result.var);
	// 	zend_function *fbc = execute_data_ptr->function_state.function;
	// 	return_value = ret->var.ptr;
	// 	return_value_ptr = (fbc->common.fn_flags & ZEND_ACC_RETURN_REFERENCE) ? &ret->var.ptr : NULL;
	// 	this_ptr = execute_data_ptr->object;
		// ht = execute_data->opline->extended_value;
	// }	

	// char *lcname;
	// int function_name_strlen, free_lcname = 0;
	// zend_class_entry *ce = NULL;
	// internal_function_handler *ih;
	// 
	// ce = ((zend_internal_function *) execute_data_ptr->function_state.function)->scope;
	// lcname = (char *)((zend_internal_function *) execute_data_ptr->function_state.function)->function_name;
	// function_name_strlen = strlen(lcname);
	
	/* handle methodcalls correctly */
	// if (ce != NULL) {
	// 	char *tmp = (char *) emalloc(function_name_strlen + 2 + ce->name_length + 1);
	// 	memcpy(tmp, ce->name, ce->name_length);
	// 	memcpy(tmp+ce->name_length, "::", 2);
	// 	memcpy(tmp+ce->name_length+2, lcname, function_name_strlen);
	// 	lcname = tmp;
	// 	free_lcname = 1;
	// 	function_name_strlen += ce->name_length + 2;
	// 	lcname[function_name_strlen] = 0;
	// 	zend_str_tolower(lcname, function_name_strlen);
	// }

	// TODO: check execute_data + ->func

	zend_string *function_name = execute_data->func->op_array.function_name;
	
	// TODO: check for function_name == NULL
	
	SDEBUG("function: %s", ZSTR_VAL(function_name));

	if (SUHOSIN7_G(in_code_type) == SUHOSIN_EVAL) {
	
		if (SUHOSIN7_G(eval_whitelist) != NULL) {
			if (!zend_hash_exists(SUHOSIN7_G(eval_whitelist), function_name)) {
				suhosin_log(S_EXECUTOR|S_GETCALLER, "function outside of eval whitelist called: %s()", ZSTR_VAL(function_name));
				if (!SUHOSIN7_G(simulation)) {
				        goto execute_internal_bailout;
				} else {
					FUNCTION_SIMULATE_WARNING()
				}
			}
		} else if (SUHOSIN7_G(eval_blacklist) != NULL) {
			if (zend_hash_exists(SUHOSIN7_G(eval_blacklist), function_name)) {
				suhosin_log(S_EXECUTOR|S_GETCALLER, "function within eval blacklist called: %s()", ZSTR_VAL(function_name));
				if (!SUHOSIN7_G(simulation)) {
				        goto execute_internal_bailout;
				} else {
					FUNCTION_SIMULATE_WARNING()
				}
			}
		}
	}
	
	if (SUHOSIN7_G(func_whitelist) != NULL) {
		if (!zend_hash_exists(SUHOSIN7_G(func_whitelist), function_name)) {
			suhosin_log(S_EXECUTOR|S_GETCALLER, "function outside of whitelist called: %s()", ZSTR_VAL(function_name));
			if (!SUHOSIN7_G(simulation)) {
				goto execute_internal_bailout;
			} else {
				FUNCTION_SIMULATE_WARNING()
			}
		}
	} else if (SUHOSIN7_G(func_blacklist) != NULL) {
		if (zend_hash_exists(SUHOSIN7_G(func_blacklist), function_name)) {
			suhosin_log(S_EXECUTOR|S_GETCALLER, "function within blacklist called: %s()", ZSTR_VAL(function_name));
			if (!SUHOSIN7_G(simulation)) {
				goto execute_internal_bailout;
			} else {
				FUNCTION_SIMULATE_WARNING()
			}
		}
	}
	
	internal_function_handler *ih;
	int retval = 0;
	if ((ih = zend_hash_find_ptr(&ihandler_table, function_name))) {
	
		void *handler = execute_data->func->internal_function.handler;
		
		if (handler != ZEND_FN(display_disabled_function)) {
		    retval = ih->handler(IH_HANDLER_PARAM_PASSTHRU);
		}
		
	}
	
	if (retval == 0) {
		old_execute_internal(execute_data, return_value);
	}

	// if (free_lcname == 1) {
	// 	efree(lcname);
	// }
	return;
execute_internal_bailout:
	// if (free_lcname == 1) {
	// 	efree(lcname);
	// }
	FUNCTION_WARNING()
	suhosin_bailout();
}
/* }}} */


/* {{{ int function_lookup(zend_extension *extension)
 */
static int function_lookup(zend_extension *extension)
{
	// if (zo_set_oe_ex != NULL) {
	// 	return ZEND_HASH_APPLY_STOP;
	// }

	// if (extension->handle != NULL) {
	// 
	// 	zo_set_oe_ex = (void *)DL_FETCH_SYMBOL(extension->handle, "zend_optimizer_set_oe_ex");
    // 
    // }

	return 0;
}
/* }}} */


/* {{{ void suhosin_hook_execute()
 */
void suhosin_hook_execute()
{
	internal_function_handler *ih;

	old_execute_ex = zend_execute_ex;
	zend_execute_ex = suhosin_execute_ex;
	
/*	old_compile_file = zend_compile_file;
	zend_compile_file = suhosin_compile_file; */

#if ZO_COMPATIBILITY_HACK_TEMPORARY_DISABLED
	if (zo_set_oe_ex == NULL) {	
		zo_set_oe_ex = (void *)DL_FETCH_SYMBOL(NULL, "zend_optimizer_set_oe_ex");
	}
	if (zo_set_oe_ex == NULL) {	
		zend_llist_apply(&zend_extensions, (llist_apply_func_t)function_lookup);
	}

	if (zo_set_oe_ex != NULL) {
		old_execute_ZO = zo_set_oe_ex(suhosin_execute_ZO);
	}
#endif
	
	old_execute_internal = zend_execute_internal;
	if (old_execute_internal == NULL) {
		old_execute_internal = execute_internal;
	}
	zend_execute_internal = suhosin_execute_internal;
	
	/* register internal function handlers */
	zend_hash_init(&ihandler_table, 16, NULL, NULL, 1);
	ih = &ihandlers[0];
	while (ih->name) {
		zend_hash_str_add_ptr(&ihandler_table, ih->name, sizeof(ih->name)-1, ih);
		ih++;
	}
		
	
	/* Add additional protection layer, that SHOULD
	   catch ZEND_INCLUDE_OR_EVAL *before* the engine tries
	   to execute */
	// old_zend_stream_open = zend_stream_open_function;
	// zend_stream_open_function = suhosin_zend_stream_open;
	
}
/* }}} */


/* {{{ void suhosin_unhook_execute()
 */
void suhosin_unhook_execute()
{
#if ZO_COMPATIBILITY_HACK_TEMPORARY_DISABLED
	if (zo_set_oe_ex) {
		zo_set_oe_ex(old_execute_ZO);
	}
#endif

	zend_execute_ex = old_execute_ex;
		
/*	zend_compile_file = old_compile_file; */

	if (old_execute_internal == execute_internal) {
		old_execute_internal = NULL;
	}
	zend_execute_internal = old_execute_internal;
	zend_hash_clean(&ihandler_table);
	
	/* remove zend_open protection */
	// zend_stream_open_function = old_zend_stream_open;
	
}
/* }}} */

// #endif // 0

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
