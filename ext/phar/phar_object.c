/*
  +----------------------------------------------------------------------+
  | phar php single-file executable PHP extension                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 2005-2007 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt.                                 |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Gregory Beaver <cellog@php.net>                             |
  |          Marcus Boerger <helly@php.net>                              |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#include "phar_internal.h"

static zend_class_entry *phar_ce_archive;
static zend_class_entry *phar_ce_PharException;

#if HAVE_SPL
static zend_class_entry *phar_ce_entry;
#endif

#ifdef PHP_WIN32
static inline void phar_unixify_path_separators(char *path, int path_len) /* {{{ */
{
	char *s;

	/* unixify win paths */
	for (s = path; s - path < path_len; s++) {
		if (*s == '\\') {
			*s = '/';
		}
	}
}
/* }}} */
#endif

static int phar_get_extract_list(void *pDest, int num_args, va_list args, zend_hash_key *hash_key) /* {{{ */
{
	zval *return_value = va_arg(args, zval*);

	add_assoc_string_ex(return_value, *(char**)&hash_key->arKey, hash_key->nKeyLength, (char*)pDest, 1);
	
	return ZEND_HASH_APPLY_KEEP;
}
/* }}} */

/* {{ proto array Phar::getExtractList()
 * Return array of extract list
 */
PHP_METHOD(Phar, getExtractList)
{
	array_init(return_value);

	phar_request_initialize(TSRMLS_C);
	zend_hash_apply_with_arguments(&PHAR_G(phar_plain_map), phar_get_extract_list, 1, return_value);
}
/* }}} */
static int phar_file_type(HashTable *mimes, char *file, char **mime_type TSRMLS_DC)
{
	char *ext;
	phar_mime_type *mime;
	if (!mime_type) {
		/* assume PHP */
		return 0;
	}
	ext = strrchr(file, '.');
	if (!ext) {
		*mime_type = "text/plain";
		/* no file extension = assume text/plain */
		return PHAR_MIME_OTHER;
	}
	ext++;
	if (SUCCESS != zend_hash_find(mimes, ext, strlen(ext), (void **) &mime)) {
		*mime_type = "application/octet-stream";
		return PHAR_MIME_OTHER;
	}
	*mime_type = mime->mime;
	return mime->type;
}
/* }}} */

static void phar_mung_server_vars(char *fname, char *entry, char *basename, int basename_len TSRMLS_DC)
{
	zval **_SERVER, **stuff;
	char *path_info;

	/* "tweak" $_SERVER variables requested in earlier call to Phar::mungServer() */
	if (!zend_hash_num_elements(&(PHAR_GLOBALS->phar_SERVER_mung_list))) {
		return;
	}
	if (SUCCESS != zend_hash_find(&EG(symbol_table), "_SERVER", sizeof("_SERVER"), (void **) &_SERVER)) {
		return;
	}
#define PHAR_MUNG_REPLACE(vname) \
	if (zend_hash_exists(&(PHAR_GLOBALS->phar_SERVER_mung_list), #vname, sizeof(#vname)-1)) { \
		if (SUCCESS == zend_hash_find(Z_ARRVAL_PP(_SERVER), #vname, sizeof(#vname), (void **) &stuff)) { \
			int code; \
			zval *temp; \
			char newname[sizeof("SCRIPT_FILENAME")+4]; \
							\
			path_info = Z_STRVAL_PP(stuff); \
			code = Z_STRLEN_PP(stuff); \
			Z_STRVAL_PP(stuff) = estrndup(Z_STRVAL_PP(stuff) + basename_len, Z_STRLEN_PP(stuff) - basename_len); \
			Z_STRLEN_PP(stuff) -= basename_len; \
							\
			MAKE_STD_ZVAL(temp); \
			Z_TYPE_P(temp) = IS_STRING; \
			Z_STRVAL_P(temp) = path_info; \
			Z_STRLEN_P(temp) = code; \
			memset(newname, 0, sizeof("SCRIPT_FILENAME")+4); \
			memcpy(newname, "PHAR_", 5); \
			memcpy(newname + 5, #vname, sizeof(#vname)); \
			zend_hash_update(Z_ARRVAL_PP(_SERVER), newname, strlen(newname)+1, (void *) &temp, sizeof(zval **), NULL); \
		} \
	}

	PHAR_MUNG_REPLACE(REQUEST_URI);
	PHAR_MUNG_REPLACE(PHP_SELF);

	if (zend_hash_exists(&(PHAR_GLOBALS->phar_SERVER_mung_list), "SCRIPT_NAME", sizeof("SCRIPT_NAME")-1)) {
		if (SUCCESS == zend_hash_find(Z_ARRVAL_PP(_SERVER), "SCRIPT_NAME", sizeof("SCRIPT_NAME"), (void **) &stuff)) { 
			int code; 
			zval *temp; 
			char newname[] = "PHAR_SCRIPT_NAME";
							
			path_info = Z_STRVAL_PP(stuff); 
			code = Z_STRLEN_PP(stuff); 
			Z_STRLEN_PP(stuff) = spprintf(&(Z_STRVAL_PP(stuff)), 4096, "phar://%s%s", fname, entry);
							
			MAKE_STD_ZVAL(temp); 
			Z_TYPE_P(temp) = IS_STRING; 
			Z_STRVAL_P(temp) = path_info; 
			Z_STRLEN_P(temp) = code; 
			zend_hash_update(Z_ARRVAL_PP(_SERVER), newname, strlen(newname)+1, (void *) &temp, sizeof(zval **), NULL); 
		} 
	}

	if (zend_hash_exists(&(PHAR_GLOBALS->phar_SERVER_mung_list), "SCRIPT_FILENAME", sizeof("SCRIPT_FILENAME")-1)) {
		if (SUCCESS == zend_hash_find(Z_ARRVAL_PP(_SERVER), "SCRIPT_FILENAME", sizeof("SCRIPT_FILENAME"), (void **) &stuff)) {
			int code; 
			zval *temp; 
			char newname[] = "PHAR_SCRIPT_FILENAME";
							
			path_info = Z_STRVAL_PP(stuff); 
			code = Z_STRLEN_PP(stuff); 
			Z_STRLEN_PP(stuff) = spprintf(&(Z_STRVAL_PP(stuff)), 4096, "phar://%s%s", fname, entry);
							
			MAKE_STD_ZVAL(temp); 
			Z_TYPE_P(temp) = IS_STRING; 
			Z_STRVAL_P(temp) = path_info; 
			Z_STRLEN_P(temp) = code; 
			zend_hash_update(Z_ARRVAL_PP(_SERVER), newname, strlen(newname)+1, (void *) &temp, sizeof(zval **), NULL); 
		}
	}
}

static int phar_file_action(phar_entry_data *phar, char *mime_type, int code, char *entry, int entry_len, char *arch, int arch_len, char *basename, int basename_len TSRMLS_DC)
{
	char *name = NULL, buf[8192];
	zend_syntax_highlighter_ini syntax_highlighter_ini;
	sapi_header_line ctr = {0};
	size_t got;
	int dummy = 1, name_len, ret;
	zend_file_handle file_handle;
	zend_op_array *new_op_array;
	zval *result = NULL;

	switch (code) {
		case PHAR_MIME_PHPS:
			efree(basename);
			/* highlight source */
			if (entry[0] == '/') {
				name_len = spprintf(&name, 4096, "phar://%s%s", arch, entry);
			} else {
				name_len = spprintf(&name, 4096, "phar://%s/%s", arch, entry);
			}
			php_get_highlight_struct(&syntax_highlighter_ini);

			if (highlight_file(name, &syntax_highlighter_ini TSRMLS_CC) == FAILURE) {
			}

			phar_entry_delref(phar TSRMLS_CC);
			efree(name);
#ifdef PHP_WIN32
			efree(arch);
#endif
			zend_bailout();
			return PHAR_MIME_PHPS;
		case PHAR_MIME_OTHER:
			/* send headers, output file contents */
			efree(basename);
			ctr.line_len = spprintf(&(ctr.line), 0, "Content-type: %s", mime_type);
			sapi_header_op(SAPI_HEADER_REPLACE, &ctr TSRMLS_CC);
			efree(ctr.line);
			ctr.line_len = spprintf(&(ctr.line), 0, "Content-length: %d", phar->internal_file->uncompressed_filesize);
			sapi_header_op(SAPI_HEADER_REPLACE, &ctr TSRMLS_CC);
			efree(ctr.line);
			if (FAILURE == sapi_send_headers(TSRMLS_C)) {
				phar_entry_delref(phar TSRMLS_CC);
				zend_bailout();
			}

			/* prepare to output  */
			if (!phar->fp) {
				char *error;
				if (!phar_open_jit(phar->phar, phar->internal_file, phar->phar->fp, &error, 0 TSRMLS_CC)) {
					if (error) {
						zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
						efree(error);
					}
					return -1;
				}
				phar->fp = phar->internal_file->fp;
				if (phar->internal_file->fp == phar->phar->fp) {
					phar->zero = phar->internal_file->offset_within_phar;
					if (!phar->is_tar && !phar->is_zip) {
						phar->zero += phar->phar->internal_file_start;
					}
				}
			}
			php_stream_seek(phar->fp, phar->zero, SEEK_SET);
			do {
				if (!phar->zero) {
					got = php_stream_read(phar->fp, buf, 8192);
					PHPWRITE(buf, got);
					if (phar->fp->eof) {
						break;
					}
				} else {
					got = php_stream_read(phar->fp, buf, MIN(8192, phar->internal_file->uncompressed_filesize - phar->position));
					PHPWRITE(buf, got);
					phar->position = php_stream_tell(phar->fp) - phar->zero;
					if (phar->position == (off_t) phar->internal_file->uncompressed_filesize) {
						break;
					}
				}
			} while (1);

			phar_entry_delref(phar TSRMLS_CC);
			zend_bailout();
			return PHAR_MIME_OTHER;
		case PHAR_MIME_PHP:
			if (basename) {
				phar_mung_server_vars(arch, entry, basename, basename_len TSRMLS_CC);
				efree(basename);
			}
			phar_entry_delref(phar TSRMLS_CC);
			if (entry[0] == '/') {
				name_len = spprintf(&name, 4096, "phar://%s%s", arch, entry);
			} else {
				name_len = spprintf(&name, 4096, "phar://%s/%s", arch, entry);
			}

			ret = php_stream_open_for_zend_ex(name, &file_handle, ENFORCE_SAFE_MODE|USE_PATH|STREAM_OPEN_FOR_INCLUDE TSRMLS_CC);
			
			if (ret != SUCCESS) {
				efree(name);
				return -1;
			}
			if (!file_handle.opened_path) {
				file_handle.opened_path = estrndup(name, name_len);
			}
			if (zend_hash_add(&EG(included_files), file_handle.opened_path, strlen(file_handle.opened_path)+1, (void *)&dummy, sizeof(int), NULL)==SUCCESS) {
				new_op_array = zend_compile_file(&file_handle, ZEND_REQUIRE TSRMLS_CC);
				zend_destroy_file_handle(&file_handle TSRMLS_CC);
			} else {
				new_op_array = NULL;
				zend_file_handle_dtor(&file_handle);
			}
#ifdef PHP_WIN32
			efree(arch);
#endif
			if (new_op_array) {
				EG(return_value_ptr_ptr) = &result;
				EG(active_op_array) = new_op_array;
		
				zend_execute(new_op_array TSRMLS_CC);
		
				destroy_op_array(new_op_array TSRMLS_CC);
				efree(new_op_array);
				if (!EG(exception)) {
					if (EG(return_value_ptr_ptr)) {
						zval_ptr_dtor(EG(return_value_ptr_ptr));
					}
				}
		
				efree(name);
				zend_bailout();
			}
			return PHAR_MIME_PHP;
	}
	return -1;
}

void phar_do_404(char *fname, int fname_len, char *f404, int f404_len TSRMLS_DC)
{
	int hi;
	phar_entry_data *phar;
	char *error;
	if (f404_len) {
		if (FAILURE == phar_get_entry_data(&phar, fname, fname_len, f404, f404_len, "r", &error TSRMLS_CC)) {
			if (error) {
				efree(error);
			}
			goto nofile;
		}
		hi = phar_file_action(phar, "text/html", PHAR_MIME_PHP, f404, f404_len, fname, fname_len, NULL, 0 TSRMLS_CC);
	} else {
		sapi_header_line ctr = {0};
nofile:
		ctr.response_code = 404;
		ctr.line_len = sizeof("HTTP/1.0 404 Not Found")+1;
		ctr.line = "HTTP/1.0 404 Not Found";
		sapi_header_op(SAPI_HEADER_REPLACE, &ctr TSRMLS_CC);
		sapi_send_headers(TSRMLS_C);
		PHPWRITE("<html><head><title>File Not Found<title></head><body><h1>404 - File Not Found</h1></body></html>", sizeof("<html><head><title>File Not Found<title></head><body><h1>404 - File Not Found</h1></body></html>") - 1);
	}
}

/* {{{ proto void Phar::webPhar([string alias, [string index, [string f404, [array mimetypes, [array rewrites]]]]])
 * mapPhar for web-based phars. Reads the currently executed file (a phar)
 * and registers its manifest. When executed in the CLI or CGI command-line sapi,
 * this works exactly like mapPhar().  When executed by a web-based sapi, this
 * reads $_SERVER['REQUEST_URI'] (the actual original value) and parses out the
 * intended internal file.
 */
PHP_METHOD(Phar, webPhar)
{
	HashTable mimetypes;
	phar_mime_type mime;
	zval *mimeoverride = NULL, *rewrites = NULL;
	char *alias = NULL, *error, *plain_map, *index_php, *f404 = NULL;
	int alias_len = 0, ret, f404_len = 0;
	char *fname, *basename, *path_info, *mime_type, *entry, *pt;
	int fname_len, entry_len, code, index_php_len = 0;
	phar_entry_data *phar;
	zval **fd_ptr;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s!s!saa", &alias, &alias_len, &index_php, &index_php_len, &f404, &f404_len, &mimeoverride, &rewrites) == FAILURE) {
		return;
	}

	phar_request_initialize(TSRMLS_C);
	if (phar_open_compiled_file(alias, alias_len, &error TSRMLS_CC) != SUCCESS) {
		if (error) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
			efree(error);
		}
		return;
	}

	/* retrieve requested file within phar */
	if (!(SG(request_info).request_method && SG(request_info).request_uri && (!strcmp(SG(request_info).request_method, "GET") || !strcmp(SG(request_info).request_method, "POST")))) {
		return;
	}

	fname = zend_get_executed_filename(TSRMLS_C);
	fname_len = strlen(fname);

	if (strstr(fname, "://")) {
		char *arch, *entry;
		int arch_len, entry_len;
		phar_archive_data *phar;

		/* running within a zip-based phar, acquire the actual name */
		if (SUCCESS != phar_split_fname(fname, fname_len, &arch, &arch_len, &entry, &entry_len TSRMLS_CC)) {
			efree(entry);
			efree(arch);
			return; /* this, incidentally, should be impossible */
		}

		efree(entry);
		entry = fname;
		fname = arch;
		fname_len = arch_len;
		if (SUCCESS == phar_open_loaded(fname, fname_len, alias, alias_len, 0, &phar, 0 TSRMLS_CC) && phar && (phar->is_zip || phar->is_tar)) {
			efree(arch);
			fname = phar->fname;
			fname_len = phar->fname_len;
		} else {
			efree(arch);
			fname = entry;
		}
	}
#ifdef PHP_WIN32
	fname = estrndup(fname, fname_len);
	phar_unixify_path_separators(fname, fname_len);
#endif
	basename = strrchr(fname, '/');
	if (!basename) {
		basename = fname;
	} else {
		basename++;
	}

	path_info = SG(request_info).request_uri;
	if (!(pt = strstr(path_info, basename))) {
		/* this can happen with rewrite rules - and we have no idea what to do then, so return */
		return;
	}
	entry_len = strlen(path_info);

	entry_len -= (pt - path_info) + (fname_len - (basename - fname));
	entry = estrndup(pt + (fname_len - (basename - fname)), entry_len);

	pt = estrndup(path_info, (pt - path_info) + (fname_len - (basename - fname)));
	if (!entry_len || (entry_len == 1 && entry[0] == '/')) {
		efree(entry);
		/* direct request */
		if (index_php_len) {
			entry = index_php;
			entry_len = index_php_len;
		} else {
			/* assume "index.php" is starting point */
			entry = estrndup("/index.php", sizeof("/index.php"));
			entry_len = sizeof("/index.php")-1;
		}
		if (FAILURE == phar_get_entry_data(&phar, fname, fname_len, entry, entry_len, "r", &error TSRMLS_CC)) {
			if (error) {
				efree(error);
			}
			phar_do_404(fname, fname_len, f404, f404_len TSRMLS_CC);
			zend_bailout();
			return;
		} else {
			char *tmp, sa, *myentry;
			sapi_header_line ctr = {0};
			ctr.response_code = 301;
			ctr.line_len = sizeof("HTTP/1.1 301 Moved Permanently")+1;
			ctr.line = "HTTP/1.1 301 Moved Permanently";
			sapi_header_op(SAPI_HEADER_REPLACE, &ctr TSRMLS_CC);

			tmp = strstr(path_info, basename) + fname_len;
			sa = *tmp;
			*tmp = '\0';
			ctr.response_code = 0;
			if (entry[0] == '/' && path_info[strlen(path_info)-1] == '/') {
				myentry = entry + 1;
			} else {
				myentry = entry;
			}
			ctr.line_len = spprintf(&(ctr.line), 4096, "Location: %s%s", path_info, myentry);
			*tmp = sa;
			sapi_header_op(SAPI_HEADER_REPLACE, &ctr TSRMLS_CC);
			sapi_send_headers(TSRMLS_C);
			phar_entry_delref(phar TSRMLS_CC);
			efree(ctr.line);
			zend_bailout();
			return;
		}
	}

	if (rewrites) {
		/* check for "rewrite" urls */
		if (SUCCESS == zend_hash_find(Z_ARRVAL_P(rewrites), entry, entry_len+1, (void **) &fd_ptr)) {
			if (IS_STRING != Z_TYPE_PP(fd_ptr)) {
				zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "phar rewrite value for \"%s\" was not a string", entry);
			}
			if (entry != index_php) {
				efree(entry);
			}
			entry = Z_STRVAL_PP(fd_ptr);
			entry_len = Z_STRLEN_PP(fd_ptr);
		}
	}

	if (FAILURE == phar_get_entry_data(&phar, fname, fname_len, entry, entry_len, "r", &error TSRMLS_CC)) {
		phar_do_404(fname, fname_len, f404, f404_len TSRMLS_CC);
#ifdef PHP_WIN32
		efree(fname);
#endif
		zend_bailout();
		return;
	}

	/* set up mime types */
	zend_hash_init(&mimetypes, sizeof(phar_mime_type *), zend_get_hash_value, NULL, 0);
#define PHAR_SET_MIME(mimetype, ret, ...) \
		mime.mime = mimetype; \
		mime.len = sizeof((mimetype))+1; \
		mime.type = ret; \
		{ \
			char mimes[][5] = {__VA_ARGS__, "\0"}; \
			int i = 0; \
			for (; mimes[i][0]; i++) { \
				zend_hash_add(&mimetypes, mimes[i], strlen(mimes[i]), (void *)&mime, sizeof(phar_mime_type), NULL); \
			} \
		}

	PHAR_SET_MIME("text/html", PHAR_MIME_PHPS, "phps")
	PHAR_SET_MIME("text/plain", PHAR_MIME_OTHER, "c", "cc", "cpp", "c++", "dtd", "h", "log", "rng", "txt", "xsd")
	PHAR_SET_MIME("", PHAR_MIME_PHP, "php", "inc")
	PHAR_SET_MIME("video/avi", PHAR_MIME_OTHER, "avi")
	PHAR_SET_MIME("image/bmp", PHAR_MIME_OTHER, "bmp")
	PHAR_SET_MIME("text/css", PHAR_MIME_OTHER, "css")
	PHAR_SET_MIME("image/gif", PHAR_MIME_OTHER, "gif")
	PHAR_SET_MIME("text/html", PHAR_MIME_OTHER, "htm", "html", "htmls")
	PHAR_SET_MIME("image/x-ico", PHAR_MIME_OTHER, "ico")
	PHAR_SET_MIME("image/jpeg", PHAR_MIME_OTHER, "jpe", "jpg", "jpeg")
	PHAR_SET_MIME("application/x-javascript", PHAR_MIME_OTHER, "js")
	PHAR_SET_MIME("audio/midi", PHAR_MIME_OTHER, "mid", "midi")
	PHAR_SET_MIME("audio/mod", PHAR_MIME_OTHER, "mod")
	PHAR_SET_MIME("movie/quicktime", PHAR_MIME_OTHER, "mov")
	PHAR_SET_MIME("audio/mp3", PHAR_MIME_OTHER, "mp3")
	PHAR_SET_MIME("video/mpeg", PHAR_MIME_OTHER, "mpg", "mpeg")
	PHAR_SET_MIME("application/pdf", PHAR_MIME_OTHER, "pdf")
	PHAR_SET_MIME("image/png", PHAR_MIME_OTHER, "png")
	PHAR_SET_MIME("application/shockwave-flash", PHAR_MIME_OTHER, "swf")
	PHAR_SET_MIME("image/tiff", PHAR_MIME_OTHER, "tif", "tiff")
	PHAR_SET_MIME("audio/wav", PHAR_MIME_OTHER, "wav")
	PHAR_SET_MIME("image/xbm", PHAR_MIME_OTHER, "xbm")
	PHAR_SET_MIME("text/xml", PHAR_MIME_OTHER, "xml")

	/* set up user overrides */
#define PHAR_SET_USER_MIME(ret) \
		mime.mime = Z_STRVAL_P(val); \
		mime.len = Z_STRLEN_P(val); \
		mime.type = ret; \
		zend_hash_update(&mimetypes, key, keylen, (void *)&mime, sizeof(phar_mime_type), NULL);

	if (mimeoverride) {
		if (!zend_hash_num_elements(Z_ARRVAL_P(mimeoverride))) {
			goto no_mimes;
		}
		for (zend_hash_internal_pointer_reset(Z_ARRVAL_P(mimeoverride)); SUCCESS == zend_hash_has_more_elements(Z_ARRVAL_P(mimeoverride)); zend_hash_move_forward(Z_ARRVAL_P(mimeoverride))) {
			zval *val;
			char *key;
			uint keylen;
			ulong intkey;
			if (HASH_KEY_IS_LONG == zend_hash_get_current_key_ex(Z_ARRVAL_P(mimeoverride), &key, &keylen, &intkey, 0, NULL)) {
				zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Key of MIME type overrides array must be a file extension, was \"%d\"", intkey);
#ifdef PHP_WIN32
				efree(fname);
#endif
				RETURN_FALSE;
			}
			if (FAILURE == zend_hash_get_current_data(Z_ARRVAL_P(mimeoverride), (void **) &val)) {
				zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Failed to retrieve Mime type for extension \"%s\"", key);
#ifdef PHP_WIN32
				efree(fname);
#endif
				RETURN_FALSE;
			}
			switch (Z_TYPE_P(val)) {
				case IS_LONG :
					if (Z_LVAL_P(val) == PHAR_MIME_PHP || Z_LVAL_P(val) == PHAR_MIME_PHPS) {
						PHAR_SET_USER_MIME(Z_LVAL_P(val))
					} else {
						zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Unknown mime type specifier used, only Phar::PHP, Phar::PHPS and a mime type string are allowed");
						RETURN_FALSE;
					}
					break;
				case IS_STRING :
					PHAR_SET_USER_MIME(PHAR_MIME_OTHER)
					break;
				default :
					zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Unknown mime type specifier used, only Phar::PHP, Phar::PHPS and a mime type string are allowed");
					RETURN_FALSE;
			}
		}
	}

no_mimes:
	code = phar_file_type(&mimetypes, entry, &mime_type TSRMLS_CC);
	ret = phar_file_action(phar, mime_type, code, entry, entry_len, fname, fname_len, pt, strlen(pt) TSRMLS_CC);
	zend_hash_destroy(&mimetypes);
#ifdef PHP_WIN32
	efree(fname);
#endif
	RETURN_LONG(ret);
}

/* {{{ proto void Phar::mungServer(array munglist)
 * Defines a list of up to 4 $_SERVER variables that should be modified for execution
 * to mask the presence of the phar archive.  This should be used in conjunction with
 * Phar::webPhar(), and has no effect otherwise
 * SCRIPT_NAME, PHP_SELF, REQUEST_URI and SCRIPT_FILENAME
 */
PHP_METHOD(Phar, mungServer)
{
	zval *mungvalues;
	int php_self = 0, request_uri = 0, script_name = 0, script_filename = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "a", &mungvalues) == FAILURE) {
		return;
	}

	if (!zend_hash_num_elements(Z_ARRVAL_P(mungvalues))) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "No values passed to Phar::mungServer(), expecting an array of any of these strings: PHP_SELF, REQUEST_URI, SCRIPT_FILENAME, SCRIPT_NAME");
		return;
	}
	if (zend_hash_num_elements(Z_ARRVAL_P(mungvalues)) > 4) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Too many values passed to Phar::mungServer(), expecting an array of any of these strings: PHP_SELF, REQUEST_URI, SCRIPT_FILENAME, SCRIPT_NAME");
		return;
	}

	for (zend_hash_internal_pointer_reset(Z_ARRVAL_P(mungvalues)); SUCCESS == zend_hash_has_more_elements(Z_ARRVAL_P(mungvalues)); zend_hash_move_forward(Z_ARRVAL_P(mungvalues))) {
		zval ***data;

		if (SUCCESS != zend_hash_get_current_data(Z_ARRVAL_P(mungvalues), (void **) data)) {
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "unable to retrieve array value in Phar::mungServer()");
			return;
		}
		if (Z_TYPE_PP(*data) != IS_STRING) {
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Non-string value passed to Phar::mungServer(), expecting an array of any of these strings: PHP_SELF, REQUEST_URI, SCRIPT_FILENAME, SCRIPT_NAME");
			return;
		}
		if (!php_self && Z_STRLEN_PP(*data) == sizeof("PHP_SELF")-1 && !strncmp(Z_STRVAL_PP(*data), "PHP_SELF", sizeof("PHP_SELF")-1)) {
			if (SUCCESS != zend_hash_add_empty_element(&(PHAR_GLOBALS->phar_SERVER_mung_list), "PHP_SELF", sizeof("PHP_SELF")-1)) {
				zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Unable to add PHP_SELF to Phar::mungServer() list of values to mung");
				return;
			}
			php_self = 1;
		}
		if (Z_STRLEN_PP(*data) == sizeof("REQUEST_URI")-1) {
			if (!request_uri && !strncmp(Z_STRVAL_PP(*data), "REQUEST_URI", sizeof("REQUEST_URI")-1)) {
				if (SUCCESS != zend_hash_add_empty_element(&(PHAR_GLOBALS->phar_SERVER_mung_list), "REQUEST_URI", sizeof("REQUEST_URI")-1)) {
					zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Unable to add REQUEST_URI to Phar::mungServer() list of values to mung");
					return;
				}
				request_uri = 1;
			}
			if (!script_name && !strncmp(Z_STRVAL_PP(*data), "SCRIPT_NAME", sizeof("SCRIPT_NAME")-1)) {
				if (SUCCESS != zend_hash_add_empty_element(&(PHAR_GLOBALS->phar_SERVER_mung_list), "SCRIPT_NAME", sizeof("SCRIPT_NAME")-1)) {
					zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Unable to add SCRIPT_NAME to Phar::mungServer() list of values to mung");
					return;
				}
				script_name = 1;
			}
		}
		if (!script_filename && Z_STRLEN_PP(*data) == sizeof("SCRIPT_FILENAME")-1 && !strncmp(Z_STRVAL_PP(*data), "SCRIPT_FILENAME", sizeof("SCRIPT_FILENAME")-1)) {
			if (SUCCESS != zend_hash_add_empty_element(&(PHAR_GLOBALS->phar_SERVER_mung_list), "SCRIPT_FILENAME", sizeof("SCRIPT_FILENAME")-1)) {
				zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Unable to add SCRIPT_FILENAME to Phar::mungServer() list of values to mung");
				return;
			}
			script_filename = 1;
		}
	}
}
/* }}} */

/* {{{ proto mixed Phar::mapPhar([string alias, [int dataoffset]])
 * Reads the currently executed file (a phar) and registers its manifest */
PHP_METHOD(Phar, mapPhar)
{
	char *fname, *alias = NULL, *error, *plain_map;
	int fname_len, alias_len = 0;
	long dataoffset;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s!l", &alias, &alias_len, &dataoffset) == FAILURE) {
		return;
	}

	phar_request_initialize(TSRMLS_C);
	if (zend_hash_num_elements(&(PHAR_GLOBALS->phar_plain_map))) {
		fname = zend_get_executed_filename(TSRMLS_C);
		fname_len = strlen(fname);
		if((alias && 
		    zend_hash_find(&(PHAR_GLOBALS->phar_plain_map), alias, alias_len+1, (void **)&plain_map) == SUCCESS)
		|| (zend_hash_find(&(PHAR_GLOBALS->phar_plain_map), fname, fname_len+1, (void **)&plain_map) == SUCCESS)
		) {
			RETURN_STRING(plain_map, 1);
		}
	}

	RETVAL_BOOL(phar_open_compiled_file(alias, alias_len, &error TSRMLS_CC) == SUCCESS);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}
} /* }}} */

/* {{{ proto mixed Phar::loadPhar(string filename [, string alias])
 * Loads any phar archive with an alias */
PHP_METHOD(Phar, loadPhar)
{
	char *fname, *alias = NULL, *error, *plain_map;
	int fname_len, alias_len = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|s!", &fname, &fname_len, &alias, &alias_len) == FAILURE) {
		return;
	}

	phar_request_initialize(TSRMLS_C);
	if (zend_hash_num_elements(&(PHAR_GLOBALS->phar_plain_map))) {
		if((alias && 
		    zend_hash_find(&(PHAR_GLOBALS->phar_plain_map), alias, alias_len+1, (void **)&plain_map) == SUCCESS)
		|| (zend_hash_find(&(PHAR_GLOBALS->phar_plain_map), fname, fname_len+1, (void **)&plain_map) == SUCCESS)
		) {
			RETURN_STRING(plain_map, 1);
		}
	}

	RETVAL_BOOL(phar_open_filename(fname, fname_len, alias, alias_len, REPORT_ERRORS, NULL, &error TSRMLS_CC) == SUCCESS);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}
} /* }}} */

/* {{{ proto string Phar::apiVersion()
 * Returns the api version */
PHP_METHOD(Phar, apiVersion)
{
	RETURN_STRINGL(PHAR_API_VERSION_STR, sizeof(PHAR_API_VERSION_STR)-1, 1);
}
/* }}}*/

/* {{{ proto bool Phar::canCompress([int method])
 * Returns whether phar extension supports compression using zlib/bzip2 */
PHP_METHOD(Phar, canCompress)
{
	long method = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &method) == FAILURE) {
		return;
	}

	switch (method) {
	case PHAR_ENT_COMPRESSED_GZ:
#if HAVE_ZLIB
		if (phar_has_zlib) {
			RETURN_TRUE;
		} else {
			RETURN_FALSE;
		}
#else
		RETURN_FALSE;
#endif

	case PHAR_ENT_COMPRESSED_BZ2:
#if HAVE_BZ2
		if (phar_has_bz2) {
			RETURN_TRUE;
		} else {
			RETURN_FALSE;
		}
#else
		RETURN_FALSE;
#endif

	default:
#if HAVE_ZLIB || HAVE_BZ2
	if (phar_has_zlib || phar_has_bz2) {
		RETURN_TRUE;
	} else {
		RETURN_FALSE;
	}
#else
		RETURN_FALSE;
#endif
	}
}
/* }}} */

/* {{{ proto bool Phar::canWrite()
 * Returns whether phar extension supports writing and creating phars */
PHP_METHOD(Phar, canWrite)
{
	RETURN_BOOL(!PHAR_G(readonly));
}
/* }}} */

/* {{{ proto bool Phar::isValidPharFilename(string filename)
 * Returns whether the given filename is a valid phar filename */
PHP_METHOD(Phar, isValidPharFilename)
{
	char *fname, *ext_str;
	int fname_len, ext_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &fname, &fname_len) == FAILURE) {
		return;
	}

	RETURN_BOOL(phar_detect_phar_fname_ext(fname, 1, &ext_str, &ext_len) == SUCCESS);
}
/* }}} */

#if HAVE_SPL
/**
 * from spl_directory
 */
static void phar_spl_foreign_dtor(spl_filesystem_object *object TSRMLS_DC) /* {{{ */
{
	phar_archive_delref((phar_archive_data *) object->oth TSRMLS_CC);
	object->oth = NULL;
}
/* }}} */

/**
 * from spl_directory
 */
static void phar_spl_foreign_clone(spl_filesystem_object *src, spl_filesystem_object *dst TSRMLS_DC) /* {{{ */
{
	phar_archive_data *phar_data = (phar_archive_data *) dst->oth;

	phar_data->refcount++;
}
/* }}} */

static spl_other_handler phar_spl_foreign_handler = {
       phar_spl_foreign_dtor,
       phar_spl_foreign_clone
};
#endif /* HAVE_SPL */

/* {{{ proto void Phar::__construct(string fname [, int flags [, string alias]])
 * Construct a Phar archive object
 */
PHP_METHOD(Phar, __construct)
{
#if !HAVE_SPL
	zend_throw_exception_ex(zend_exception_get_default(TSRMLS_C), 0 TSRMLS_CC, "Cannot instantiate Phar object without SPL extension");
#else
	char *fname, *alias = NULL, *error;
	int fname_len, alias_len = 0;
	long flags = 0;
	phar_archive_object *phar_obj;
	phar_archive_data   *phar_data;
	zval *zobj = getThis(), arg1, arg2;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|ls!", &fname, &fname_len, &flags, &alias, &alias_len) == FAILURE) {
		return;
	}

	phar_obj = (phar_archive_object*)zend_object_store_get_object(getThis() TSRMLS_CC);

	if (phar_obj->arc.archive) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Cannot call constructor twice");
		return;
	}

	if (phar_open_or_create_filename(fname, fname_len, alias, alias_len, REPORT_ERRORS, &phar_data, &error TSRMLS_CC) == FAILURE) {
		if (error) {
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"Cannot open phar file '%s' with alias '%s': %s", fname, alias, error);
			efree(error);
		} else {
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"Cannot open phar file '%s' with alias '%s'", fname, alias);
		}
		return;
	}

	phar_data->refcount++;
	phar_obj->arc.archive = phar_data;
	phar_obj->spl.oth_handler = &phar_spl_foreign_handler;

#ifdef PHP_WIN32
	phar_unixify_path_separators(fname, fname_len);
#endif

	fname_len = spprintf(&fname, 0, "phar://%s", fname);
	INIT_PZVAL(&arg1);
	ZVAL_STRINGL(&arg1, fname, fname_len, 0);

	if (ZEND_NUM_ARGS() > 1) {
		INIT_PZVAL(&arg2);
		ZVAL_LONG(&arg2, flags);
		zend_call_method_with_2_params(&zobj, Z_OBJCE_P(zobj), 
			&spl_ce_RecursiveDirectoryIterator->constructor, "__construct", NULL, &arg1, &arg2);
	} else {
		zend_call_method_with_1_params(&zobj, Z_OBJCE_P(zobj), 
			&spl_ce_RecursiveDirectoryIterator->constructor, "__construct", NULL, &arg1);
	}

	phar_obj->spl.info_class = phar_ce_entry;

	efree(fname);
#endif /* HAVE_SPL */
}
/* }}} */

/* {{{ proto array Phar::getSupportedSignatures()
 * Return array of supported signature types
 */
PHP_METHOD(Phar, getSupportedSignatures)
{
	array_init(return_value);

	add_next_index_stringl(return_value, "MD5", 3, 1);
	add_next_index_stringl(return_value, "SHA-1", 5, 1);
#if HAVE_HASH_EXT
	add_next_index_stringl(return_value, "SHA-256", 7, 1);
	add_next_index_stringl(return_value, "SHA-512", 7, 1);
#endif
}
/* }}} */

/* {{{ proto array Phar::getSupportedCompression()
 * Return array of supported comparession algorithms
 */
PHP_METHOD(Phar, getSupportedCompression)
{
	array_init(return_value);

#if HAVE_ZLIB
	if (phar_has_zlib) {
		add_next_index_stringl(return_value, "GZ", 2, 1);
	}
#endif
#if HAVE_BZ2
	if (phar_has_bz2) {
		add_next_index_stringl(return_value, "BZIP2", 5, 1);
	}
#endif
}
/* }}} */

#if HAVE_SPL

#define PHAR_ARCHIVE_OBJECT() \
	phar_archive_object *phar_obj = (phar_archive_object*)zend_object_store_get_object(getThis() TSRMLS_CC); \
	if (!phar_obj->arc.archive) { \
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
			"Cannot call method on an uninitialized Phar object"); \
		return; \
	}

static int phar_build(zend_object_iterator *iter, void *puser TSRMLS_DC)
{
	zval **value;
	zend_uchar key_type;
	zend_bool is_splfileinfo = 0, close_fp = 1;
	ulong int_key;
	struct _t {
		phar_archive_object *p;
		zend_class_entry *c;
		char *b;
		uint l;
		zval *ret;
	} *p_obj = (struct _t*) puser;
	uint str_key_len, base_len = p_obj->l, fname_len;
	phar_entry_data *data;
	php_stream *fp;
	long contents_len;
	char *fname, *error, *str_key, *base = p_obj->b, *opened, *save = NULL;
	zend_class_entry *ce = p_obj->c;
	phar_archive_object *phar_obj = p_obj->p;
	char *str = "[stream]";

	iter->funcs->get_current_data(iter, &value TSRMLS_CC);
	if (EG(exception)) {
		return ZEND_HASH_APPLY_STOP;
	}
	if (!value) {
		/* failure in get_current_data */
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned no value", ce->name);
	}
	switch (Z_TYPE_PP(value)) {
		case IS_STRING :
			break;
		case IS_RESOURCE :
			php_stream_from_zval_no_verify(fp, value);
			if (!fp) {
				zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Iterator %s returned an invalid stream handle", ce->name);
				return ZEND_HASH_APPLY_STOP;
			}
			if (iter->funcs->get_current_key) {
				key_type = iter->funcs->get_current_key(iter, &str_key, &str_key_len, &int_key TSRMLS_CC);
				if (EG(exception)) {
					return ZEND_HASH_APPLY_STOP;
				}
				if (key_type == HASH_KEY_IS_LONG) {
					zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned an invalid key (must return a string)", ce->name);
					return ZEND_HASH_APPLY_STOP;
				}
				save = str_key;
				if (str_key[str_key_len - 1] == '\0') str_key_len--;
			} else {
				zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned an invalid key (must return a string)", ce->name);
				return ZEND_HASH_APPLY_STOP;
			}
			close_fp = 0;
			opened = (char *) estrndup(str, sizeof("[stream]") + 1);
			goto after_open_fp;
		case IS_OBJECT :
			if (instanceof_function(Z_OBJCE_PP(value), spl_ce_SplFileInfo TSRMLS_CC)) {
				char *test = NULL;
				zval dummy;
				spl_filesystem_object *intern = (spl_filesystem_object*)zend_object_store_get_object(*value TSRMLS_CC);

				if (!base_len) {
					zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Iterator %s returns an SplFileInfo object, so base directory must be specified", ce->name);
					return ZEND_HASH_APPLY_STOP;
				}
				switch (intern->type) {
					case SPL_FS_DIR:
						fname_len = spprintf(&fname, 0, "%s%c%s", intern->path, DEFAULT_SLASH, intern->u.dir.entry.d_name);
						php_stat(fname, fname_len, FS_IS_DIR, &dummy TSRMLS_CC);
						if (Z_BVAL(dummy)) {
							/* ignore directories */
							efree(fname);
							return ZEND_HASH_APPLY_KEEP;
						}
						test = expand_filepath(fname, test TSRMLS_CC);
						if (test) {
							efree(fname);
							fname = test;
							fname_len = strlen(fname);
						}
						save = fname;
						is_splfileinfo = 1;
						goto phar_spl_fileinfo;
					case SPL_FS_INFO:
					case SPL_FS_FILE:
						return ZEND_HASH_APPLY_KEEP;
				}
			}
			/* fall-through */
		default :
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned an invalid value (must return a string)", ce->name);
			return ZEND_HASH_APPLY_STOP;
	}

	fname = Z_STRVAL_PP(value);
	fname_len = Z_STRLEN_PP(value);

phar_spl_fileinfo:
	if (base_len) {
		if (strstr(fname, base)) {
			str_key_len = fname_len - base_len;
			if (str_key_len <= 0) {
				if (save) {
					efree(save);
				}
				return ZEND_HASH_APPLY_KEEP;
			}
			str_key = fname + base_len;
		} else {
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned a path \"%s\" that is not in the base directory \"%s\"", ce->name, fname, base);
			if (save) {
				efree(save);
			}
			return ZEND_HASH_APPLY_STOP;
		}
	} else {
		if (iter->funcs->get_current_key) {
			key_type = iter->funcs->get_current_key(iter, &str_key, &str_key_len, &int_key TSRMLS_CC);
			if (EG(exception)) {
				return ZEND_HASH_APPLY_STOP;
			}
			if (key_type == HASH_KEY_IS_LONG) {
				zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned an invalid key (must return a string)", ce->name);
				return ZEND_HASH_APPLY_STOP;
			}
			save = str_key;
			if (str_key[str_key_len - 1] == '\0') str_key_len--;
		} else {
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned an invalid key (must return a string)", ce->name);
			return ZEND_HASH_APPLY_STOP;
		}
	}
#if PHP_MAJOR_VERSION < 6
	if (PG(safe_mode) && (!php_checkuid(fname, NULL, CHECKUID_ALLOW_ONLY_FILE))) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned a path \"%s\" that safe mode prevents opening", ce->name, fname);
		if (save) {
			efree(save);
		}
		return ZEND_HASH_APPLY_STOP;
	}
#endif

	if (php_check_open_basedir(fname TSRMLS_CC)) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned a path \"%s\" that open_basedir prevents opening", ce->name, fname);
		if (save) {
			efree(save);
		}
		return ZEND_HASH_APPLY_STOP;
	}

	/* try to open source file, then create internal phar file and copy contents */
	fp = php_stream_open_wrapper(fname, "rb", STREAM_MUST_SEEK|0, &opened);
	if (!fp) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC, "Iterator %s returned a file that could not be opened \"%s\"", ce->name, fname);
		if (save) {
			efree(save);
		}
		return ZEND_HASH_APPLY_STOP;
	}

after_open_fp:
	if (!(data = phar_get_or_create_entry_data(phar_obj->arc.archive->fname, phar_obj->arc.archive->fname_len, str_key, str_key_len, "w+b", &error TSRMLS_CC))) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Entry %s cannot be created: %s", str_key, error);
		efree(error);
		if (save) {
			efree(save);
		}
		if (close_fp) {
			php_stream_close(fp);
		}
		return ZEND_HASH_APPLY_STOP;
	} else {
		if (error) {
			efree(error);
		}
		contents_len = php_stream_copy_to_stream(fp, data->fp, PHP_STREAM_COPY_ALL);
	}
	if (close_fp) {
		php_stream_close(fp);
	}

	add_assoc_string(p_obj->ret, str_key, opened, 0);
	if (save) {
		efree(save);
	}

	data->internal_file->compressed_filesize = data->internal_file->uncompressed_filesize = contents_len;
	phar_entry_delref(data TSRMLS_CC);
	return ZEND_HASH_APPLY_KEEP;
}

/* {{{ proto array Phar::buildFromIterator(Iterator iter[, string base_directory])
 * Construct a phar archive from an iterator.  The iterator must return a series of strings
 * that are full paths to files that should be added to the phar.  The iterator key should
 * be the path that the file will have within the phar archive.
 *
 * If base directory is specified, then the key will be ignored, and instead the portion of
 * the current value minus the base directory will be used
 *
 * Returned is an array mapping phar index to actual file added
 */
PHP_METHOD(Phar, buildFromIterator)
{
	zval *obj;
	char *error;
	uint base_len = 0;
	char *base;
	struct {
		phar_archive_object *p;
		zend_class_entry *c;
		char *b;
		uint l;
		zval *ret;
	} pass;
	PHAR_ARCHIVE_OBJECT();

	if (PHAR_G(readonly)) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot write out phar archive, phar is read-only");
		return;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O|s", &obj, zend_ce_traversable, &base, &base_len) == FAILURE) {
		RETURN_FALSE;
	}

	array_init(return_value);

	pass.c = Z_OBJCE_P(obj);
	pass.p = phar_obj;
	pass.b = base;
	pass.l = base_len;
	pass.ret = return_value;

	if (SUCCESS == spl_iterator_apply(obj, (spl_iterator_apply_func_t) phar_build, (void *) &pass TSRMLS_CC)) {
		phar_flush(phar_obj->arc.archive, 0, 0, &error TSRMLS_CC);
		if (error) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
			efree(error);
		}
	}

}
/* }}} */

/* {{{ proto int Phar::count()
 * Returns the number of entries in the Phar archive
 */
PHP_METHOD(Phar, count)
{
	PHAR_ARCHIVE_OBJECT();
	
	RETURN_LONG(zend_hash_num_elements(&phar_obj->arc.archive->manifest));
}
/* }}} */

/* {{{ proto bool Phar::delete(string file)
 * Delete a file from within the Phar
 */
PHP_METHOD(Phar, delete)
{
	char *fname;
	int fname_len;
	char *error;
	phar_entry_info *entry;
	PHAR_ARCHIVE_OBJECT();

	if (PHAR_G(readonly)) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot write out phar archive, phar is read-only");
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &fname, &fname_len) == FAILURE) {
		RETURN_FALSE;
	}

	if (zend_hash_exists(&phar_obj->arc.archive->manifest, fname, (uint) fname_len)) {
		if (SUCCESS == zend_hash_find(&phar_obj->arc.archive->manifest, fname, (uint) fname_len, (void**)&entry)) {
			if (entry->is_deleted) {
				/* entry is deleted, but has not been flushed to disk yet */
				RETURN_TRUE;
			} else {
				entry->is_deleted = 1;
				entry->is_modified = 1;
				phar_obj->arc.archive->is_modified = 1;
			}
		}
	} else {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Entry %s does not exist and cannot be deleted", fname);
		RETURN_FALSE;
	}

	phar_flush(phar_obj->arc.archive, NULL, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}
		
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto int Phar::getAlias()
 * Returns the alias for the PHAR or NULL
 */
PHP_METHOD(Phar, getAlias)
{
	PHAR_ARCHIVE_OBJECT();

	if (phar_obj->arc.archive->alias && phar_obj->arc.archive->alias != phar_obj->arc.archive->fname) {
		RETURN_STRINGL(phar_obj->arc.archive->alias, phar_obj->arc.archive->alias_len, 1);
	}
}
/* }}} */

/* {{{ proto bool Phar::setAlias(string alias)
 * Set the alias for the PHAR
 */
PHP_METHOD(Phar, setAlias)
{
	char *alias, *error;
	phar_archive_data *fd, **fd_ptr;
	int alias_len;
	PHAR_ARCHIVE_OBJECT();

	if (PHAR_G(readonly)) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot write out phar archive, phar is read-only");
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &alias, &alias_len) == SUCCESS) {
		if (alias_len == phar_obj->arc.archive->alias_len && memcmp(phar_obj->arc.archive->alias, alias, alias_len) == 0) {
			RETURN_TRUE;
		}
		if (SUCCESS == zend_hash_find(&(PHAR_GLOBALS->phar_alias_map), alias, alias_len, (void**)&fd_ptr)) {
			spprintf(&error, 0, "alias \"%s\" is already used for archive \"%s\" and cannot be used for other archives", alias, (*fd_ptr)->fname);
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
			efree(error);
			RETURN_FALSE;
		}
		if (SUCCESS == zend_hash_find(&(PHAR_GLOBALS->phar_alias_map), phar_obj->arc.archive->alias, phar_obj->arc.archive->alias_len, (void**)&fd_ptr)) {
			zend_hash_del(&(PHAR_GLOBALS->phar_alias_map), alias, alias_len);
			fd = *fd_ptr;
			if (alias && alias_len) {
				zend_hash_add(&(PHAR_GLOBALS->phar_alias_map), alias, alias_len, (void*)&fd,   sizeof(phar_archive_data*), NULL);
			}
		}

		efree(phar_obj->arc.archive->alias);
		if (alias_len) {
			phar_obj->arc.archive->alias = estrndup(alias, alias_len);
		} else {
			phar_obj->arc.archive->alias = NULL;
		}
		phar_obj->arc.archive->alias_len = alias_len;
		phar_obj->arc.archive->is_explicit_alias = 0;
		phar_flush(phar_obj->arc.archive, NULL, 0, &error TSRMLS_CC);
		if (error) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
			efree(error);
		}
		RETURN_TRUE;
	}

	RETURN_FALSE;
}

/* {{{ proto string Phar::getVersion()
 * Return version info of Phar archive
 */
PHP_METHOD(Phar, getVersion)
{
	PHAR_ARCHIVE_OBJECT();
	
	RETURN_STRING(phar_obj->arc.archive->version, 1);
}
/* }}} */

/* {{{ proto void Phar::startBuffering()
 * Do not flush a writeable phar (save its contents) until explicitly requested
 */
PHP_METHOD(Phar, startBuffering)
{
	PHAR_ARCHIVE_OBJECT();
	
	phar_obj->arc.archive->donotflush = 1;
}
/* }}} */

/* {{{ proto bool Phar::isBuffering()
 * Returns whether write operations are flushing to disk immediately
 */
PHP_METHOD(Phar, isBuffering)
{
	PHAR_ARCHIVE_OBJECT();
	
	RETURN_BOOL(!phar_obj->arc.archive->donotflush);
}
/* }}} */

/* {{{ proto bool Phar::stopBuffering()
 * Save the contents of a modified phar
 */
PHP_METHOD(Phar, stopBuffering)
{
	char *error;
	PHAR_ARCHIVE_OBJECT();

	if (PHAR_G(readonly)) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot write out phar archive, phar is read-only");
	}

	phar_obj->arc.archive->donotflush = 0;

	phar_flush(phar_obj->arc.archive, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}
}
/* }}} */

/* {{{ proto bool Phar::setStub(string|stream stub [, int len])
 * Change the stub of the archive
 */
PHP_METHOD(Phar, setStub)
{
	zval *zstub;
	char *stub, *error;
	int stub_len;
	long len = -1;
	php_stream *stream;
	PHAR_ARCHIVE_OBJECT();

	if (PHAR_G(readonly)) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot change stub, phar is read-only");
	}

	if (zend_parse_parameters_ex(ZEND_PARSE_PARAMS_QUIET, ZEND_NUM_ARGS() TSRMLS_CC, "r|l", &zstub, &len) == SUCCESS) {
		if ((php_stream_from_zval_no_verify(stream, &zstub)) != NULL) {
			if (len > 0) {
				len = -len;
			} else {
				len = -1;
			}
			phar_flush(phar_obj->arc.archive, (char *) &zstub, len, &error TSRMLS_CC);
			if (error) {
				zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
				efree(error);
			}
			RETURN_TRUE;
		} else {
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"Cannot change stub, unable to read from input stream");
		}
	} else if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &stub, &stub_len) == SUCCESS) {
		phar_flush(phar_obj->arc.archive, stub, stub_len, &error TSRMLS_CC);
		if (error) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
			efree(error);
		}
		RETURN_TRUE;
	}

	RETURN_FALSE;
}
/* }}} */

/* {{{ proto array Phar::setSignatureAlgorithm(int sigtype)
 * set the signature algorithm for a phar and apply it.  The
 * signature algorithm must be one of Phar::MD5, Phar::SHA1,
 * Phar::SHA256, Phar::SHA512, or Phar::PGP (pgp not yet supported and
 * falls back to SHA-1)
 */
PHP_METHOD(Phar, setSignatureAlgorithm)
{
	long algo;
	char *error;
	PHAR_ARCHIVE_OBJECT();
	
	if (PHAR_G(readonly)) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot set signature algorithm, phar is read-only");
	}
	if (phar_obj->arc.archive->is_tar) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot set signature algorithm, not possible with tar-based phar archives");
	}

	if (zend_parse_parameters_ex(ZEND_PARSE_PARAMS_QUIET, ZEND_NUM_ARGS() TSRMLS_CC, "l", &algo) != SUCCESS) {
		return;
	}

	switch (algo) {
		case PHAR_SIG_SHA256 :
		case PHAR_SIG_SHA512 :
#if !HAVE_HASH_EXT
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"SHA-256 and SHA-512 signatures are only supported if the hash extension is enabled");
#endif
		case PHAR_SIG_MD5 :
		case PHAR_SIG_SHA1 :
		case PHAR_SIG_PGP :
			phar_obj->arc.archive->sig_flags = algo;
			phar_obj->arc.archive->is_modified = 1;

			phar_flush(phar_obj->arc.archive, 0, 0, &error TSRMLS_CC);
			if (error) {
				zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, error);
				efree(error);
			}
			break;
		default :
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"Unknown signature algorithm specified");
	}
}
/* }}} */

/* {{{ proto array|false Phar::getSignature()
 * Return signature or false
 */
PHP_METHOD(Phar, getSignature)
{
	PHAR_ARCHIVE_OBJECT();

	if (phar_obj->arc.archive->signature) {
		array_init(return_value);
		add_assoc_stringl(return_value, "hash", phar_obj->arc.archive->signature, phar_obj->arc.archive->sig_len, 1);
		switch(phar_obj->arc.archive->sig_flags) {
		case PHAR_SIG_MD5:
			add_assoc_stringl(return_value, "hash_type", "MD5", 3, 1);
			break;
		case PHAR_SIG_SHA1:
			add_assoc_stringl(return_value, "hash_type", "SHA-1", 5, 1);
			break;
		case PHAR_SIG_SHA256:
			add_assoc_stringl(return_value, "hash_type", "SHA-256", 7, 1);
			break;
		case PHAR_SIG_SHA512:
			add_assoc_stringl(return_value, "hash_type", "SHA-512", 7, 1);
			break;
		}
	} else {
		RETURN_FALSE;
	}
}
/* }}} */

/* {{{ proto bool Phar::getModified()
 * Return whether phar was modified
 */
PHP_METHOD(Phar, getModified)
{
	PHAR_ARCHIVE_OBJECT();

	RETURN_BOOL(phar_obj->arc.archive->is_modified);
}
/* }}} */

static int phar_set_compression(void *pDest, void *argument TSRMLS_DC) /* {{{ */
{
	phar_entry_info *entry = (phar_entry_info *)pDest;
	php_uint32 compress = *(php_uint32 *)argument;

	if (entry->is_deleted) {
		return ZEND_HASH_APPLY_KEEP;
	}
	entry->old_flags = entry->flags;
	entry->flags &= ~PHAR_ENT_COMPRESSION_MASK;
	entry->flags |= compress;
	entry->is_modified = 1;
	return ZEND_HASH_APPLY_KEEP;
}
/* }}} */

static int phar_test_compression(void *pDest, void *argument TSRMLS_DC) /* {{{ */
{
	phar_entry_info *entry = (phar_entry_info *)pDest;

	if (entry->is_deleted) {
		return ZEND_HASH_APPLY_KEEP;
	}
#if !HAVE_BZ2
	if (entry->flags & PHAR_ENT_COMPRESSED_BZ2) {
		*(int *) argument = 0;
	}
#else
	if (!phar_has_bz2) {
		if (entry->flags & PHAR_ENT_COMPRESSED_BZ2) {
			*(int *) argument = 0;
		}
	}
#endif
#if !HAVE_ZLIB
	if (entry->flags & PHAR_ENT_COMPRESSED_GZ) {
		*(int *) argument = 0;
	}
#else
	if (!phar_has_zlib) {
		if (entry->flags & PHAR_ENT_COMPRESSED_GZ) {
			*(int *) argument = 0;
		}
	}
#endif
	return ZEND_HASH_APPLY_KEEP;
}
/* }}} */

static void pharobj_set_compression(HashTable *manifest, php_uint32 compress TSRMLS_DC) /* {{{ */
{
	zend_hash_apply_with_argument(manifest, phar_set_compression, &compress TSRMLS_CC);
}
/* }}} */

static int pharobj_cancompress(HashTable *manifest TSRMLS_DC) /* {{{ */
{
	int test;
	test = 1;
	zend_hash_apply_with_argument(manifest, phar_test_compression, &test TSRMLS_CC);
	return test;
}
/* }}} */

/* {{{ proto bool Phar::compressAllFilesGZ()
 * compress every file with GZip compression
 */
PHP_METHOD(Phar, compressAllFilesGZ)
{
#if HAVE_ZLIB
	char *error;
#endif
	PHAR_ARCHIVE_OBJECT();

	if (phar_obj->arc.archive->is_tar) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot compress all files as Gzip, not possible with tar-based phar archives");
	}
	if (PHAR_G(readonly)) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Phar is readonly, cannot change compression");
	}
#if HAVE_ZLIB
	if (!phar_has_zlib) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot compress with Gzip compression, zlib extension is not enabled");
	}
	if (!pharobj_cancompress(&phar_obj->arc.archive->manifest TSRMLS_CC)) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot compress all files as Gzip, some are compressed as bzip2 and cannot be uncompressed");
	}
	pharobj_set_compression(&phar_obj->arc.archive->manifest, PHAR_ENT_COMPRESSED_GZ TSRMLS_CC);
	phar_obj->arc.archive->is_modified = 1;
	
	phar_flush(phar_obj->arc.archive, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, error);
		efree(error);
	}
#else
	zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
		"Cannot compress with Gzip compression, zlib extension is not enabled");
#endif
}
/* }}} */

/* {{{ proto bool Phar::compressAllFilesBZIP2()
 * compress every file with BZip2 compression
 */
PHP_METHOD(Phar, compressAllFilesBZIP2)
{
#if HAVE_BZ2
	char *error;
#endif
	PHAR_ARCHIVE_OBJECT();

	if (phar_obj->arc.archive->is_tar) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot compress all files as Bzip2, not possible with tar-based phar archives");
	}
	if (phar_obj->arc.archive->is_zip) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot compress all files as Bzip2, not possible with zip-based phar archives");
	}

	if (PHAR_G(readonly)) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Phar is readonly, cannot change compression");
	}
#if HAVE_BZ2
	if (!phar_has_bz2) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot compress with Bzip2 compression, bz2 extension is not enabled");
	}
	if (!pharobj_cancompress(&phar_obj->arc.archive->manifest TSRMLS_CC)) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot compress all files as Bzip2, some are compressed as gzip and cannot be uncompressed");
	}
	pharobj_set_compression(&phar_obj->arc.archive->manifest, PHAR_ENT_COMPRESSED_BZ2 TSRMLS_CC);
	phar_obj->arc.archive->is_modified = 1;
	
	phar_flush(phar_obj->arc.archive, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, error);
		efree(error);
	}
#else
	zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
		"Cannot compress with Bzip2 compression, bz2 extension is not enabled");
#endif
}
/* }}} */

/* {{{ proto bool Phar::uncompressAllFiles()
 * uncompress every file
 */
PHP_METHOD(Phar, uncompressAllFiles)
{
	char *error;
	PHAR_ARCHIVE_OBJECT();

	if (PHAR_G(readonly)) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Phar is readonly, cannot change compression");
	}
	if (!pharobj_cancompress(&phar_obj->arc.archive->manifest TSRMLS_CC)) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot uncompress all files, some are compressed as bzip2 or gzip and cannot be uncompressed");
	}
	pharobj_set_compression(&phar_obj->arc.archive->manifest, PHAR_ENT_COMPRESSED_NONE TSRMLS_CC);
	phar_obj->arc.archive->is_modified = 1;
	
	phar_flush(phar_obj->arc.archive, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, error);
		efree(error);
	}
}
/* }}} */

/* {{{ proto bool Phar::copy(string oldfile, string newfile)
 * copy a file internal to the phar archive to another new file within the phar
 */
PHP_METHOD(Phar, copy)
{
	char *oldfile, *newfile, *error;
	const char *pcr_error;
	int oldfile_len, newfile_len;
	phar_entry_info *oldentry, newentry = {0}, *temp;
	php_stream *fp;
	PHAR_ARCHIVE_OBJECT();

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &oldfile, &oldfile_len, &newfile, &newfile_len) == FAILURE) {
		return;
	}

	if (PHAR_G(readonly)) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot copy \"%s\" to \"%s\", phar is read-only", oldfile, newfile);
		RETURN_FALSE;
	}

	if (!zend_hash_exists(&phar_obj->arc.archive->manifest, oldfile, (uint) oldfile_len)) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"file \"%s\" does not exist in phar %s", oldfile, phar_obj->arc.archive->fname);
		RETURN_FALSE;
	}

	if (zend_hash_exists(&phar_obj->arc.archive->manifest, newfile, (uint) newfile_len)) {
		if (SUCCESS == zend_hash_find(&phar_obj->arc.archive->manifest, newfile, (uint) newfile_len, (void**)&temp) || !temp->is_deleted) {
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"file \"%s\" cannot be copied to file \"%s\", file must not already exist %s", oldfile, newfile, phar_obj->arc.archive->fname);
			RETURN_FALSE;
		}
	}

	if (phar_path_check(&newfile, &newfile_len, &pcr_error) > pcr_is_ok) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"file \"%s\" contains invalid characters %s, cannot be copied from \"%s\" in phar %s", newfile, pcr_error, oldfile, phar_obj->arc.archive->fname);
		RETURN_FALSE;
	}

	if (!zend_hash_exists(&phar_obj->arc.archive->manifest, oldfile, (uint) oldfile_len) || SUCCESS != zend_hash_find(&phar_obj->arc.archive->manifest, oldfile, (uint) oldfile_len, (void**)&oldentry) || oldentry->is_deleted) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"file \"%s\" cannot be copied to file \"%s\", file does not exist in %s", oldfile, newfile, phar_obj->arc.archive->fname);
		RETURN_FALSE;
	}

	fp = oldentry->fp;
	if (fp && fp != phar_obj->arc.archive->fp) {
		/* new file */
		newentry.fp = php_stream_temp_new();
		fp = newentry.fp;
		php_stream_seek(fp, 0, SEEK_SET);
		if (oldentry->compressed_filesize != php_stream_copy_to_stream(oldentry->fp, fp, oldentry->compressed_filesize)) {
			php_stream_close(fp);
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"file \"%s\" could not be copied to file \"%s\" in %s, copy failed", oldfile, newfile, phar_obj->arc.archive->fname);
		}
	}

	memcpy((void *) &newentry, oldentry, sizeof(phar_entry_info));
	if (newentry.metadata) {
		SEPARATE_ZVAL(&(newentry.metadata));
		newentry.metadata_str.c = NULL;
		newentry.metadata_str.len = 0;
	}
#if HAVE_PHAR_ZIP
	if (oldentry->is_zip) {
		newentry.index = -1;
	}
#endif
	newentry.fp = fp;
	newentry.filename = estrndup(newfile, newfile_len);
	newentry.filename_len = newfile_len;
	if (oldentry->is_tar) {
		newentry.tar_type = oldentry->tar_type;
	}
	phar_obj->arc.archive->is_modified = 1;
	zend_hash_add(&phar_obj->arc.archive->manifest, newfile, newfile_len, (void*)&newentry, sizeof(phar_entry_info), NULL);

	phar_flush(phar_obj->arc.archive, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}

	RETURN_TRUE;
}

/* {{{ proto int Phar::offsetExists(string offset)
 * determines whether a file exists in the phar
 */
PHP_METHOD(Phar, offsetExists)
{
	char *fname;
	int fname_len;
	phar_entry_info *entry;
	PHAR_ARCHIVE_OBJECT();

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &fname, &fname_len) == FAILURE) {
		return;
	}

	if (zend_hash_exists(&phar_obj->arc.archive->manifest, fname, (uint) fname_len)) {
		if (SUCCESS == zend_hash_find(&phar_obj->arc.archive->manifest, fname, (uint) fname_len, (void**)&entry)) {
			if (entry->is_deleted) {
				/* entry is deleted, but has not been flushed to disk yet */
				RETURN_FALSE;
			}
		}
		RETURN_TRUE;
	} else {
		RETURN_FALSE;
	}
}
/* }}} */

/* {{{ proto int Phar::offsetGet(string offset)
 * get a PharFileInfo object for a specific file
 */
PHP_METHOD(Phar, offsetGet)
{
	char *fname, *error;
	int fname_len;
	zval *zfname;
	PHAR_ARCHIVE_OBJECT();

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &fname, &fname_len) == FAILURE) {
		return;
	}
	
	if (!phar_get_entry_info(phar_obj->arc.archive, fname, fname_len, &error TSRMLS_CC)) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Entry %s does not exist%s%s", fname, error?", ":"", error?error:"");
	} else {
		fname_len = spprintf(&fname, 0, "phar://%s/%s", phar_obj->arc.archive->fname, fname);
		MAKE_STD_ZVAL(zfname);
		ZVAL_STRINGL(zfname, fname, fname_len, 0);
		spl_instantiate_arg_ex1(phar_obj->spl.info_class, &return_value, 0, zfname TSRMLS_CC);
		zval_ptr_dtor(&zfname);
	}

}
/* }}} */

/* {{{ proto int Phar::offsetSet(string offset, string value)
 * set the contents of an internal file to those of an external file
 */
PHP_METHOD(Phar, offsetSet)
{
	char *fname, *cont_str = NULL, *error;
	int fname_len, cont_len;
	zval *zresource;
	long contents_len;
	phar_entry_data *data;
	php_stream *contents_file;
	PHAR_ARCHIVE_OBJECT();

	if (PHAR_G(readonly)) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Write operations disabled by INI setting");
		return;
	}
	
	if (zend_parse_parameters_ex(ZEND_PARSE_PARAMS_QUIET, ZEND_NUM_ARGS() TSRMLS_CC, "sr", &fname, &fname_len, &zresource) == FAILURE
	&& zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &fname, &fname_len, &cont_str, &cont_len) == FAILURE) {
		return;
	}

	if (!(data = phar_get_or_create_entry_data(phar_obj->arc.archive->fname, phar_obj->arc.archive->fname_len, fname, fname_len, "w+b", &error TSRMLS_CC))) {
		if (error) {
			zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Entry %s does not exist and cannot be created: %s", fname, error);
			efree(error);
		} else {
			zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Entry %s does not exist and cannot be created", fname);
		}
	} else {
		if (error) {
			efree(error);
		}
		if (cont_str) {
			contents_len = php_stream_write(data->fp, cont_str, cont_len);
			if (contents_len != cont_len) {
				zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Entry %s could not be written to", fname);
			}
		} else {
			if (!(php_stream_from_zval_no_verify(contents_file, &zresource))) {
				zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Entry %s could not be written to", fname);
			}
			contents_len = php_stream_copy_to_stream(contents_file, data->fp, PHP_STREAM_COPY_ALL);
		}
		data->internal_file->compressed_filesize = data->internal_file->uncompressed_filesize = contents_len;
		phar_entry_delref(data TSRMLS_CC);
		phar_flush(phar_obj->arc.archive, 0, 0, &error TSRMLS_CC);
		if (error) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
			efree(error);
		}
	}
}
/* }}} */

/* {{{ proto int Phar::offsetUnset()
 * remove a file from a phar
 */
PHP_METHOD(Phar, offsetUnset)
{
	char *fname, *error;
	int fname_len;
	phar_entry_info *entry;
	PHAR_ARCHIVE_OBJECT();

	if (PHAR_G(readonly)) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Write operations disabled by INI setting");
		return;
	}
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &fname, &fname_len) == FAILURE) {
		return;
	}

	if (zend_hash_exists(&phar_obj->arc.archive->manifest, fname, (uint) fname_len)) {
		if (SUCCESS == zend_hash_find(&phar_obj->arc.archive->manifest, fname, (uint) fname_len, (void**)&entry)) {
			if (entry->is_deleted) {
				/* entry is deleted, but has not been flushed to disk yet */
				return;
			}
			entry->is_modified = 0;
			entry->is_deleted = 1;
#if HAVE_PHAR_ZIP
			if (entry->is_zip) {
				if (entry->zip) {
					zip_fclose(entry->zip);
					entry->zip = 0;
				}
				zip_delete(phar_obj->arc.archive->zip, entry->index);
			}
#endif
			/* we need to "flush" the stream to save the newly deleted file on disk */
			phar_flush(phar_obj->arc.archive, 0, 0, &error TSRMLS_CC);
			if (error) {
				zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
				efree(error);
			}
			RETURN_TRUE;
		}
	} else {
		RETURN_FALSE;
	}
}
/* }}} */

/* {{{ proto string Phar::getStub()
 * Get the pre-phar stub
 */
PHP_METHOD(Phar, getStub)
{
	char *buf;
	size_t len;
	php_stream *fp;
	PHAR_ARCHIVE_OBJECT();

#if HAVE_PHAR_ZIP
	if (phar_obj->arc.archive->is_zip) {
		struct zip_stat zs;
		struct zip_file *zf;
		int index;

		if (-1 == zip_stat(phar_obj->arc.archive->zip, ".phar/stub.php", 0, &zs)) {
			zip_error_clear(phar_obj->arc.archive->zip);
			RETURN_STRINGL("", 0, 1);
		}
		index = zs.index;
		len = zs.size;
		zf = zip_fopen_index(phar_obj->arc.archive->zip, index, 0);
		if (!zf) {
			zip_error_clear(phar_obj->arc.archive->zip);
			RETURN_STRINGL("", 0, 1);
		}
		buf = safe_emalloc(len, 1, 1);
		if (len != zip_fread(zf, buf, len)) {
			zip_fclose(zf);
			efree(buf);
			zend_throw_exception_ex(spl_ce_RuntimeException, 0 TSRMLS_CC,
				"Unable to read stub");
			return;
		}
		buf[len] = '\0';

		RETURN_STRINGL(buf, len, 0);
	}
#endif
	if (phar_obj->arc.archive->is_tar) {
		phar_entry_info *stub;

		if (SUCCESS == zend_hash_find(&(phar_obj->arc.archive->manifest), ".phar/stub.php", sizeof(".phar/stub.php")-1, (void **)&stub)) {
			if (phar_obj->arc.archive->fp && !phar_obj->arc.archive->is_brandnew) {
				fp = phar_obj->arc.archive->fp;
			} else {
				fp = php_stream_open_wrapper(phar_obj->arc.archive->fname, "rb", 0, NULL);
			}

			if (!fp)  {
				zend_throw_exception_ex(spl_ce_RuntimeException, 0 TSRMLS_CC,
					"Unable to read stub");
				return;
			}

			php_stream_seek(fp, stub->offset_within_phar, SEEK_SET);
			len = stub->uncompressed_filesize;
			goto carry_on;
		} else {
			RETURN_STRINGL("", 0, 0);
		}
	}
	len = phar_obj->arc.archive->halt_offset;

	if (phar_obj->arc.archive->fp && !phar_obj->arc.archive->is_brandnew) {
		fp = phar_obj->arc.archive->fp;
	} else {
		fp = php_stream_open_wrapper(phar_obj->arc.archive->fname, "rb", 0, NULL);
	}

	if (!fp)  {
		zend_throw_exception_ex(spl_ce_RuntimeException, 0 TSRMLS_CC,
			"Unable to read stub");
		return;
	}

	php_stream_rewind(fp);
carry_on:
	buf = safe_emalloc(len, 1, 1);
	if (len != php_stream_read(fp, buf, len)) {
		if (fp != phar_obj->arc.archive->fp) {
			php_stream_close(fp);
		}
		zend_throw_exception_ex(spl_ce_RuntimeException, 0 TSRMLS_CC,
			"Unable to read stub");
		efree(buf);
		return;
	}
	if (fp != phar_obj->arc.archive->fp) {
		php_stream_close(fp);
	}
	buf[len] = '\0';

	RETURN_STRINGL(buf, len, 0);
}
/* }}}*/

/* {{{ proto int Phar::hasMetaData()
 * Returns whether phar has global metadata
 */
PHP_METHOD(Phar, hasMetadata)
{
	PHAR_ARCHIVE_OBJECT();

	RETURN_BOOL(phar_obj->arc.archive->metadata != NULL);
}
/* }}} */

/* {{{ proto int Phar::getMetaData()
 * Returns the global metadata of the phar
 */
PHP_METHOD(Phar, getMetadata)
{
	PHAR_ARCHIVE_OBJECT();

	if (phar_obj->arc.archive->metadata) {
		RETURN_ZVAL(phar_obj->arc.archive->metadata, 1, 0);
	}
}
/* }}} */

/* {{{ proto int Phar::setMetaData(mixed $metadata)
 * Sets the global metadata of the phar
 */
PHP_METHOD(Phar, setMetadata)
{
	char *error;
	zval *metadata;
	PHAR_ARCHIVE_OBJECT();

	if (phar_obj->arc.archive->is_tar) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot set metadata, not possible with tar-based phar archives");
	}
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &metadata) == FAILURE) {
		return;
	}

	if (phar_obj->arc.archive->metadata) {
		zval_ptr_dtor(&phar_obj->arc.archive->metadata);
		phar_obj->arc.archive->metadata = NULL;
	}

	MAKE_STD_ZVAL(phar_obj->arc.archive->metadata);
	ZVAL_ZVAL(phar_obj->arc.archive->metadata, metadata, 1, 0);

	phar_flush(phar_obj->arc.archive, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}
}
/* }}} */

/* {{{ proto int Phar::delMetaData()
 * Deletes the global metadata of the phar
 */
PHP_METHOD(Phar, delMetadata)
{
	char *error;
	PHAR_ARCHIVE_OBJECT();

	if (phar_obj->arc.archive->metadata) {
		zval_ptr_dtor(&phar_obj->arc.archive->metadata);
		phar_obj->arc.archive->metadata = NULL;

		phar_flush(phar_obj->arc.archive, 0, 0, &error TSRMLS_CC);
		if (error) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
			efree(error);
			RETURN_FALSE;
		} else {
			RETURN_TRUE;
		}
	} else {
		RETURN_TRUE;
	}
}
/* }}} */

/* {{{ proto void PharFileInfo::__construct(string entry)
 * Construct a Phar entry object
 */
PHP_METHOD(PharFileInfo, __construct)
{
	char *fname, *arch, *entry, *error;
	int fname_len, arch_len, entry_len;
	phar_entry_object  *entry_obj;
	phar_entry_info    *entry_info;
	phar_archive_data *phar_data;
	zval *zobj = getThis(), arg1;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &fname, &fname_len) == FAILURE) {
		return;
	}
	
	entry_obj = (phar_entry_object*)zend_object_store_get_object(getThis() TSRMLS_CC);

	if (entry_obj->ent.entry) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, "Cannot call constructor twice");
		return;
	}

	if (phar_split_fname(fname, fname_len, &arch, &arch_len, &entry, &entry_len TSRMLS_CC) == FAILURE) {
		efree(arch);
		efree(entry);
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot access phar file entry '%s'", fname);
		return;
	}

	if (phar_open_filename(arch, arch_len, NULL, 0, REPORT_ERRORS, &phar_data, &error TSRMLS_CC) == FAILURE) {
		efree(arch);
		efree(entry);
		if (error) {
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"Cannot open phar file '%s': %s", fname, error);
			efree(error);
		} else {
			zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
				"Cannot open phar file '%s'", fname);
		}
		return;
	}

	if ((entry_info = phar_get_entry_info_dir(phar_data, entry, entry_len, 1, &error TSRMLS_CC)) == NULL) {
		zend_throw_exception_ex(spl_ce_UnexpectedValueException, 0 TSRMLS_CC,
			"Cannot access phar file entry '%s' in archive '%s'%s%s", entry, arch, error?", ":"", error?error:"");
		efree(arch);
		efree(entry);
		return;
	}

	efree(arch);
	efree(entry);

	entry_obj->ent.entry = entry_info;

	INIT_PZVAL(&arg1);
	ZVAL_STRINGL(&arg1, fname, fname_len, 0);

	zend_call_method_with_1_params(&zobj, Z_OBJCE_P(zobj), 
		&spl_ce_SplFileInfo->constructor, "__construct", NULL, &arg1);
}
/* }}} */

#define PHAR_ENTRY_OBJECT() \
	phar_entry_object *entry_obj = (phar_entry_object*)zend_object_store_get_object(getThis() TSRMLS_CC); \
	if (!entry_obj->ent.entry) { \
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
			"Cannot call method on an uninitialized PharFileInfo object"); \
		return; \
	}

/* {{{ proto void PharFileInfo::__destruct()
 * clean up directory-based entry objects
 */
PHP_METHOD(PharFileInfo, __destruct)
{
	PHAR_ENTRY_OBJECT();

	if (entry_obj->ent.entry->is_dir) {
		if (!entry_obj->ent.entry->is_zip && !entry_obj->ent.entry->is_tar && entry_obj->ent.entry->filename) {
			efree(entry_obj->ent.entry->filename);
			entry_obj->ent.entry->filename = NULL;
		}
		efree(entry_obj->ent.entry);
		entry_obj->ent.entry = NULL;
	}
}
/* }}} */

/* {{{ proto int PharFileInfo::getCompressedSize()
 * Returns the compressed size
 */
PHP_METHOD(PharFileInfo, getCompressedSize)
{
	PHAR_ENTRY_OBJECT();

	RETURN_LONG(entry_obj->ent.entry->compressed_filesize);
}
/* }}} */

/* {{{ proto bool PharFileInfo::isCompressed()
 * Returns whether the entry is compressed
 */
PHP_METHOD(PharFileInfo, isCompressed)
{
	PHAR_ENTRY_OBJECT();
	
	RETURN_BOOL(entry_obj->ent.entry->flags & PHAR_ENT_COMPRESSION_MASK);
}
/* }}} */

/* {{{ proto bool PharFileInfo::isCompressedGZ()
 * Returns whether the entry is compressed using gz
 */
PHP_METHOD(PharFileInfo, isCompressedGZ)
{
	PHAR_ENTRY_OBJECT();
	
	RETURN_BOOL(entry_obj->ent.entry->flags & PHAR_ENT_COMPRESSED_GZ);
}
/* }}} */

/* {{{ proto bool PharFileInfo::isCompressedBZIP2()
 * Returns whether the entry is compressed using bzip2
 */
PHP_METHOD(PharFileInfo, isCompressedBZIP2)
{
	PHAR_ENTRY_OBJECT();
	
	RETURN_BOOL(entry_obj->ent.entry->flags & PHAR_ENT_COMPRESSED_BZ2);
}
/* }}} */

/* {{{ proto int PharFileInfo::getCRC32()
 * Returns CRC32 code or throws an exception if not CRC checked
 */
PHP_METHOD(PharFileInfo, getCRC32)
{
	PHAR_ENTRY_OBJECT();

	if (entry_obj->ent.entry->is_dir) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
			"Phar entry is a directory, does not have a CRC"); \
	}
	if (entry_obj->ent.entry->is_crc_checked) {
		RETURN_LONG(entry_obj->ent.entry->crc32);
	} else {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
			"Phar entry was not CRC checked"); \
	}
}
/* }}} */

/* {{{ proto int PharFileInfo::isCRCChecked()
 * Returns whether file entry is CRC checked
 */
PHP_METHOD(PharFileInfo, isCRCChecked)
{
	PHAR_ENTRY_OBJECT();
	
	RETURN_BOOL(entry_obj->ent.entry->is_crc_checked);
}
/* }}} */

/* {{{ proto int PharFileInfo::getPharFlags()
 * Returns the Phar file entry flags
 */
PHP_METHOD(PharFileInfo, getPharFlags)
{
	PHAR_ENTRY_OBJECT();
	
	RETURN_LONG(entry_obj->ent.entry->flags & ~(PHAR_ENT_PERM_MASK|PHAR_ENT_COMPRESSION_MASK));
}
/* }}} */

/* {{{ proto int PharFileInfo::chmod()
 * set the file permissions for the Phar.  This only allows setting execution bit, read/write
 */
PHP_METHOD(PharFileInfo, chmod)
{
	char *error;
	long perms;
	PHAR_ENTRY_OBJECT();

	if (entry_obj->ent.entry->is_dir) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
			"Phar entry is a directory, cannot chmod"); \
	}
	if (PHAR_G(readonly)) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, "Cannot modify permissions for file \"%s\" write operations are prohibited", entry_obj->ent.entry->filename, entry_obj->ent.entry->phar->fname);
	}
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &perms) == FAILURE) {
		return;
	}	
	/* clear permissions */ 
	entry_obj->ent.entry->flags &= ~PHAR_ENT_PERM_MASK;
	perms &= 0777;
	entry_obj->ent.entry->flags |= perms;
	entry_obj->ent.entry->old_flags = entry_obj->ent.entry->flags;
	entry_obj->ent.entry->phar->is_modified = 1;
	entry_obj->ent.entry->is_modified = 1;
	/* hackish cache in php_stat needs to be cleared */
	/* if this code fails to work, check main/streams/streams.c, _php_stream_stat_path */
	if (BG(CurrentLStatFile)) {
		efree(BG(CurrentLStatFile));
	}
	if (BG(CurrentStatFile)) {
		efree(BG(CurrentStatFile));
	}
	BG(CurrentLStatFile) = NULL;
	BG(CurrentStatFile) = NULL;

	phar_flush(entry_obj->ent.entry->phar, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}
}
/* }}} */

/* {{{ proto int PharFileInfo::hasMetaData()
 * Returns the metadata of the entry
 */
PHP_METHOD(PharFileInfo, hasMetadata)
{
	PHAR_ENTRY_OBJECT();
	
	RETURN_BOOL(entry_obj->ent.entry->metadata != NULL);
}
/* }}} */

/* {{{ proto int PharFileInfo::getMetaData()
 * Returns the metadata of the entry
 */
PHP_METHOD(PharFileInfo, getMetadata)
{
	PHAR_ENTRY_OBJECT();
	
	if (entry_obj->ent.entry->metadata) {
		RETURN_ZVAL(entry_obj->ent.entry->metadata, 1, 0);
	}
}
/* }}} */

/* {{{ proto int PharFileInfo::setMetaData(mixed $metadata)
 * Sets the metadata of the entry
 */
PHP_METHOD(PharFileInfo, setMetadata)
{
	char *error;
	zval *metadata;
	PHAR_ENTRY_OBJECT();

	if (entry_obj->ent.entry->is_tar) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot set metadata, not possible with tar-based phar archives");
	}
	if (entry_obj->ent.entry->is_dir) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
			"Phar entry is a directory, cannot set metadata"); \
	}
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &metadata) == FAILURE) {
		return;
	}

	if (entry_obj->ent.entry->metadata) {
		zval_ptr_dtor(&entry_obj->ent.entry->metadata);
		entry_obj->ent.entry->metadata = NULL;
	}

	MAKE_STD_ZVAL(entry_obj->ent.entry->metadata);
	ZVAL_ZVAL(entry_obj->ent.entry->metadata, metadata, 1, 0);

	phar_flush(entry_obj->ent.entry->phar, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}
}
/* }}} */

/* {{{ proto bool PharFileInfo::delMetaData()
 * Deletes the metadata of the entry
 */
PHP_METHOD(PharFileInfo, delMetadata)
{
	char *error;
	PHAR_ENTRY_OBJECT();

	if (entry_obj->ent.entry->is_dir) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
			"Phar entry is a directory, cannot delete metadata"); \
	}
	if (entry_obj->ent.entry->metadata) {
		zval_ptr_dtor(&entry_obj->ent.entry->metadata);
		entry_obj->ent.entry->metadata = NULL;

		phar_flush(entry_obj->ent.entry->phar, 0, 0, &error TSRMLS_CC);
		if (error) {
			zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
			efree(error);
			RETURN_FALSE;
		} else {
			RETURN_TRUE;
		}
	} else {
		RETURN_TRUE;
	}
}
/* }}} */

/* {{{ proto int PharFileInfo::setCompressedGZ()
 * Instructs the Phar class to compress the current file using zlib
 */
PHP_METHOD(PharFileInfo, setCompressedGZ)
{
#if HAVE_ZLIB
	char *error;
	PHAR_ENTRY_OBJECT();

	if (entry_obj->ent.entry->is_tar) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot compress with Gzip compression, not possible with tar-based phar archives");
	}
	if (entry_obj->ent.entry->is_dir) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
			"Phar entry is a directory, cannot set compression"); \
	}
	if (entry_obj->ent.entry->flags & PHAR_ENT_COMPRESSED_GZ) {
		RETURN_TRUE;
		return;
	}
	if (PHAR_G(readonly)) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Phar is readonly, cannot change compression");
	}
	if (entry_obj->ent.entry->is_deleted) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot compress deleted file");
	}
	if (!phar_has_zlib) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot compress with Gzip compression, zlib extension is not enabled");
	}
	entry_obj->ent.entry->old_flags = entry_obj->ent.entry->flags;
	entry_obj->ent.entry->flags &= ~PHAR_ENT_COMPRESSION_MASK;
	entry_obj->ent.entry->flags |= PHAR_ENT_COMPRESSED_GZ;
	entry_obj->ent.entry->phar->is_modified = 1;
	entry_obj->ent.entry->is_modified = 1;

	phar_flush(entry_obj->ent.entry->phar, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}
	RETURN_TRUE;
#else
	zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
		"Cannot compress with Gzip compression, zlib extension is not enabled");
#endif
}
/* }}} */

/* {{{ proto int PharFileInfo::setCompressedBZIP2()
 * Instructs the Phar class to compress the current file using bzip2
 */
PHP_METHOD(PharFileInfo, setCompressedBZIP2)
{
#if HAVE_BZ2
	char *error;
	PHAR_ENTRY_OBJECT();

	if (entry_obj->ent.entry->is_zip) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot compress with Bzip2 compression, not possible with zip-based phar archives");
	}
	if (entry_obj->ent.entry->is_tar) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot compress with Bzip2 compression, not possible with tar-based phar archives");
	}
	if (!phar_has_bz2) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot compress with Bzip2 compression, bz2 extension is not enabled");
	}
	if (entry_obj->ent.entry->is_dir) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
			"Phar entry is a directory, cannot set compression"); \
	}
	if (entry_obj->ent.entry->flags & PHAR_ENT_COMPRESSED_BZ2) {
		RETURN_TRUE;
		return;
	}
	if (PHAR_G(readonly)) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Phar is readonly, cannot change compression");
	}
	if (entry_obj->ent.entry->is_deleted) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot compress deleted file");
	}
	entry_obj->ent.entry->old_flags = entry_obj->ent.entry->flags;
	entry_obj->ent.entry->flags &= ~PHAR_ENT_COMPRESSION_MASK;
	entry_obj->ent.entry->flags |= PHAR_ENT_COMPRESSED_BZ2;
	entry_obj->ent.entry->phar->is_modified = 1;
	entry_obj->ent.entry->is_modified = 1;

	phar_flush(entry_obj->ent.entry->phar, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}
	RETURN_TRUE;
#else
	zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
		"Cannot compress with Bzip2 compression, bzip2 extension is not enabled");
#endif
}
/* }}} */

/* {{{ proto int PharFileInfo::setUncompressed()
 * Instructs the Phar class to uncompress the current file
 */
PHP_METHOD(PharFileInfo, setUncompressed)
{
	char *fname, *error;
	int fname_len;
	PHAR_ENTRY_OBJECT();

	if (entry_obj->ent.entry->is_dir) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC, \
			"Phar entry is a directory, cannot set compression"); \
	}
	if ((entry_obj->ent.entry->flags & PHAR_ENT_COMPRESSION_MASK) == 0) {
		RETURN_TRUE;
		return;
	}
	if (PHAR_G(readonly)) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Phar is readonly, cannot change compression");
	}
	if (entry_obj->ent.entry->is_deleted) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot compress deleted file");
	}
#if !HAVE_ZLIB
	if (entry_obj->ent.entry->flags & PHAR_ENT_COMPRESSED_GZ) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot uncompress Gzip-compressed file, zlib extension is not enabled");
	}
#else
	if (!phar_has_zlib) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot uncompress Gzip-compressed file, zlib extension is not enabled");
	}
#endif
#if !HAVE_BZ2
	if (entry_obj->ent.entry->flags & PHAR_ENT_COMPRESSED_BZ2) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot uncompress Bzip2-compressed file, bzip2 extension is not enabled");
	}
#else
	if (!phar_has_bz2) {
		zend_throw_exception_ex(spl_ce_BadMethodCallException, 0 TSRMLS_CC,
			"Cannot uncompress Bzip2-compressed file, bzip2 extension is not enabled");
	}
#endif
	if (!entry_obj->ent.entry->fp) {
		fname_len = spprintf(&fname, 0, "phar://%s/%s", entry_obj->ent.entry->phar->fname, entry_obj->ent.entry->filename);
		entry_obj->ent.entry->fp = php_stream_open_wrapper_ex(fname, "rb", 0, 0, 0);
		efree(fname);
	}
	entry_obj->ent.entry->old_flags = entry_obj->ent.entry->flags;
	entry_obj->ent.entry->flags &= ~PHAR_ENT_COMPRESSION_MASK;
	entry_obj->ent.entry->phar->is_modified = 1;
	entry_obj->ent.entry->is_modified = 1;

	phar_flush(entry_obj->ent.entry->phar, 0, 0, &error TSRMLS_CC);
	if (error) {
		zend_throw_exception_ex(phar_ce_PharException, 0 TSRMLS_CC, error);
		efree(error);
	}
	RETURN_TRUE;
}
/* }}} */

#endif /* HAVE_SPL */

/* {{{ phar methods */

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar___construct, 0, 0, 1)
	ZEND_ARG_INFO(0, fname)
	ZEND_ARG_INFO(0, flags)
	ZEND_ARG_INFO(0, alias)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_mapPhar, 0, 0, 0)
	ZEND_ARG_INFO(0, alias)
	ZEND_ARG_INFO(0, offset)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_webPhar, 0, 0, 0)
	ZEND_ARG_INFO(0, alias)
	ZEND_ARG_INFO(0, index)
	ZEND_ARG_INFO(0, f404)
	ZEND_ARG_INFO(0, mimetypes)
	ZEND_ARG_INFO(0, rewrites)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_mungServer, 0, 0, 1)
	ZEND_ARG_INFO(0, munglist)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_setStub, 0, 0, 1)
	ZEND_ARG_INFO(0, newstub)
	ZEND_ARG_INFO(0, maxlen)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_loadPhar, 0, 0, 1)
	ZEND_ARG_INFO(0, fname)
	ZEND_ARG_INFO(0, alias)
ZEND_END_ARG_INFO();

#if HAVE_SPL
static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_offsetExists, 0, 0, 1)
	ZEND_ARG_INFO(0, entry)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_offsetSet, 0, 0, 2)
	ZEND_ARG_INFO(0, entry)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_build, 0, 0, 1)
	ZEND_ARG_INFO(0, iterator)
	ZEND_ARG_INFO(0, base_directory)
ZEND_END_ARG_INFO();

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_phar_copy, 0, 0, 2)
	ZEND_ARG_INFO(0, newfile)
	ZEND_ARG_INFO(0, oldfile)
ZEND_END_ARG_INFO();
#endif

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_entry_setMetadata, 0, 0, 1)
	ZEND_ARG_INFO(0, metadata)
ZEND_END_ARG_INFO();

zend_function_entry php_archive_methods[] = {
#if !HAVE_SPL
	PHP_ME(Phar, __construct,           arginfo_phar___construct,  ZEND_ACC_PRIVATE)
#else
	PHP_ME(Phar, __construct,           arginfo_phar___construct,  ZEND_ACC_PUBLIC)
	PHP_ME(Phar, startBuffering,        NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, stopBuffering,         NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, compressAllFilesGZ,    NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, compressAllFilesBZIP2, NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, copy,                  arginfo_phar_copy,         ZEND_ACC_PUBLIC)
	PHP_ME(Phar, count,                 NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, delete,                arginfo_phar_mapPhar,      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, delMetadata,           NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, getAlias,              NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, getMetadata,           NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, getModified,           NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, getSignature,          NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, getStub,               NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, getVersion,            NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, isBuffering,           NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, hasMetadata,           NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, setAlias,              arginfo_phar_mapPhar,      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, setMetadata,           arginfo_entry_setMetadata, ZEND_ACC_PUBLIC)
	PHP_ME(Phar, setStub,               arginfo_phar_setStub,      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, setSignatureAlgorithm, arginfo_entry_setMetadata, ZEND_ACC_PUBLIC)
	PHP_ME(Phar, offsetExists,          arginfo_phar_offsetExists, ZEND_ACC_PUBLIC)
	PHP_ME(Phar, offsetGet,             arginfo_phar_offsetExists, ZEND_ACC_PUBLIC)
	PHP_ME(Phar, offsetSet,             arginfo_phar_offsetSet,    ZEND_ACC_PUBLIC)
	PHP_ME(Phar, offsetUnset,           arginfo_phar_offsetExists, ZEND_ACC_PUBLIC)
	PHP_ME(Phar, uncompressAllFiles,    NULL,                      ZEND_ACC_PUBLIC)
	PHP_ME(Phar, buildFromIterator,     arginfo_phar_build,        ZEND_ACC_PUBLIC)
#endif
	/* static member functions */
	PHP_ME(Phar, apiVersion,            NULL,                      ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, canCompress,           NULL,                      ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, canWrite,              NULL,                      ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, loadPhar,              arginfo_phar_loadPhar,     ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, mapPhar,               arginfo_phar_mapPhar,      ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, webPhar,               arginfo_phar_webPhar,      ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, mungServer,            arginfo_phar_mungServer,   ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, getExtractList,        NULL,                      ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, getSupportedSignatures,NULL,                      ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, getSupportedCompression,NULL,                     ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(Phar, isValidPharFilename,   NULL,                      ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	{NULL, NULL, NULL}
};

#if HAVE_SPL
static
ZEND_BEGIN_ARG_INFO_EX(arginfo_entry___construct, 0, 0, 1)
	ZEND_ARG_INFO(0, fname)
	ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO();

zend_function_entry php_entry_methods[] = {
	PHP_ME(PharFileInfo, __construct,        arginfo_entry___construct,  0)
	PHP_ME(PharFileInfo, __destruct,         NULL,                       0)
	PHP_ME(PharFileInfo, getCompressedSize,  NULL,                       0)
	PHP_ME(PharFileInfo, isCompressed,       NULL,                       0)
	PHP_ME(PharFileInfo, isCompressedGZ,     NULL,                       0)
	PHP_ME(PharFileInfo, isCompressedBZIP2,  NULL,                       0)
	PHP_ME(PharFileInfo, setUncompressed,    NULL,                       0)
	PHP_ME(PharFileInfo, setCompressedGZ,    NULL,                       0)
	PHP_ME(PharFileInfo, setCompressedBZIP2, NULL,                       0)
	PHP_ME(PharFileInfo, getCRC32,           NULL,                       0)
	PHP_ME(PharFileInfo, isCRCChecked,       NULL,                       0)
	PHP_ME(PharFileInfo, getPharFlags,       NULL,                       0)
	PHP_ME(PharFileInfo, hasMetadata,        NULL,                       0)
	PHP_ME(PharFileInfo, getMetadata,        NULL,                       0)
	PHP_ME(PharFileInfo, setMetadata,        arginfo_entry_setMetadata,  0)
	PHP_ME(PharFileInfo, delMetadata,        NULL,                       0)
	PHP_ME(PharFileInfo, chmod,              arginfo_entry_setMetadata,  0)
	{NULL, NULL, NULL}
};
#endif

zend_function_entry phar_exception_methods[] = {
	{NULL, NULL, NULL}
};
/* }}} */

#define REGISTER_PHAR_CLASS_CONST_LONG(class_name, const_name, value) \
	zend_declare_class_constant_long(class_name, const_name, sizeof(const_name)-1, (long)value TSRMLS_CC);

#if PHP_VERSION_ID < 50200
# define phar_exception_get_default() zend_exception_get_default()
#else
# define phar_exception_get_default() zend_exception_get_default(TSRMLS_C)
#endif

void phar_object_init(TSRMLS_D) /* {{{ */
{
	zend_class_entry ce;

	INIT_CLASS_ENTRY(ce, "PharException", phar_exception_methods);
	phar_ce_PharException = zend_register_internal_class_ex(&ce, phar_exception_get_default(), NULL  TSRMLS_CC);

#if HAVE_SPL
	INIT_CLASS_ENTRY(ce, "Phar", php_archive_methods);
	phar_ce_archive = zend_register_internal_class_ex(&ce, spl_ce_RecursiveDirectoryIterator, NULL  TSRMLS_CC);

	zend_class_implements(phar_ce_archive TSRMLS_CC, 2, spl_ce_Countable, zend_ce_arrayaccess);

	INIT_CLASS_ENTRY(ce, "PharFileInfo", php_entry_methods);
	phar_ce_entry = zend_register_internal_class_ex(&ce, spl_ce_SplFileInfo, NULL  TSRMLS_CC);
#else
	INIT_CLASS_ENTRY(ce, "Phar", php_archive_methods);
	phar_ce_archive = zend_register_internal_class(&ce TSRMLS_CC);
	phar_ce_archive->ce_flags |= ZEND_ACC_FINAL_CLASS;
#endif

	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "COMPRESSED", PHAR_ENT_COMPRESSION_MASK)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "GZ", PHAR_ENT_COMPRESSED_GZ)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "BZ2", PHAR_ENT_COMPRESSED_BZ2)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "MD5", PHAR_SIG_MD5)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "SHA1", PHAR_SIG_SHA1)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "SHA256", PHAR_SIG_SHA256)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "SHA512", PHAR_SIG_SHA512)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "PGP", PHAR_SIG_PGP)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "PHP", PHAR_MIME_PHP)
	REGISTER_PHAR_CLASS_CONST_LONG(phar_ce_archive, "PHPS", PHAR_MIME_PHPS)
}
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
