/* This is a generated file, edit the .stub.php file instead.
 * Stub hash: 571596a6888d66145a5b0d1b8391a4fa7d88506d */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_deepclone_to_array, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, allowedClasses, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_deepclone_from_array, 0, 1, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, data, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, allowedClasses, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_FUNCTION(deepclone_to_array);
ZEND_FUNCTION(deepclone_from_array);

static const zend_function_entry ext_functions[] = {
	ZEND_FE(deepclone_to_array, arginfo_deepclone_to_array)
	ZEND_FE(deepclone_from_array, arginfo_deepclone_from_array)
	ZEND_FE_END
};

static zend_class_entry *register_class_DeepClone_NotInstantiableException(zend_class_entry *class_entry_InvalidArgumentException)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "DeepClone", "NotInstantiableException", NULL);
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_InvalidArgumentException, 0);

	return class_entry;
}

static zend_class_entry *register_class_DeepClone_ClassNotFoundException(zend_class_entry *class_entry_InvalidArgumentException)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "DeepClone", "ClassNotFoundException", NULL);
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_InvalidArgumentException, 0);

	return class_entry;
}
