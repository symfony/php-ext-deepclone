#ifndef PHP_DEEPCLONE_H
#define PHP_DEEPCLONE_H

extern zend_module_entry deepclone_module_entry;
#define phpext_deepclone_ptr &deepclone_module_entry

#define PHP_DEEPCLONE_VERSION "0.2.0"

ZEND_BEGIN_MODULE_GLOBALS(deepclone)
	HashTable hydrate_cache;
	/* Per-request cache of pre-constructed ReflectionProperty instances, keyed
	 * by zend_property_info pointer. Used to amortize the construction cost in
	 * the DEEPCLONE_HYDRATE_NO_LAZY_INIT path. */
	HashTable lazy_init_refl_cache;
ZEND_END_MODULE_GLOBALS(deepclone)

ZEND_EXTERN_MODULE_GLOBALS(deepclone)

#define DC_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(deepclone, v)

#if defined(ZTS) && defined(COMPILE_DL_DEEPCLONE)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

#endif
