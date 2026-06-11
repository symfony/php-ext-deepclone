/* This is a generated file, edit deepclone.stub.php instead.
 * Stub hash: ec11b6f3b9b69f77cbddece18176eab2ffd1007a */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_deepclone_to_array, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, allowed_classes, IS_ARRAY, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, allow_named_closures, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_deepclone_from_array, 0, 1, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, data, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, allowed_classes, IS_ARRAY, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, allow_named_closures, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_deepclone_hydrate, 0, 1, IS_OBJECT, 0)
	ZEND_ARG_TYPE_MASK(0, object_or_class, MAY_BE_OBJECT|MAY_BE_STRING, NULL)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, vars, IS_ARRAY, 0, "[]")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, flags, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_DeepClone_HydrationContext___construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DeepClone_HydrationContext_hydrate, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, object, IS_OBJECT, 0)
ZEND_END_ARG_INFO()

ZEND_FUNCTION(deepclone_to_array);
ZEND_FUNCTION(deepclone_from_array);
ZEND_FUNCTION(deepclone_hydrate);
ZEND_METHOD(DeepClone_HydrationContext, __construct);
ZEND_METHOD(DeepClone_HydrationContext, hydrate);

static const zend_function_entry ext_functions[] = {
	ZEND_FE(deepclone_to_array, arginfo_deepclone_to_array)
	ZEND_FE(deepclone_from_array, arginfo_deepclone_from_array)
	ZEND_FE(deepclone_hydrate, arginfo_deepclone_hydrate)
	ZEND_FE_END
};

static const zend_function_entry class_DeepClone_HydrationContext_methods[] = {
	ZEND_ME(DeepClone_HydrationContext, __construct, arginfo_class_DeepClone_HydrationContext___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(DeepClone_HydrationContext, hydrate, arginfo_class_DeepClone_HydrationContext_hydrate, ZEND_ACC_PRIVATE)
	ZEND_FE_END
};

static void register_deepclone_symbols(int module_number)
{
	REGISTER_LONG_CONSTANT("DEEPCLONE_HYDRATE_CALL_HOOKS", DEEPCLONE_HYDRATE_CALL_HOOKS, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("DEEPCLONE_HYDRATE_NO_LAZY_INIT", DEEPCLONE_HYDRATE_NO_LAZY_INIT, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("DEEPCLONE_HYDRATE_PRESERVE_REFS", DEEPCLONE_HYDRATE_PRESERVE_REFS, CONST_PERSISTENT);
}

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

static zend_class_entry *register_class_DeepClone_HydrationContext(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "DeepClone", "HydrationContext", class_DeepClone_HydrationContext_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES|ZEND_ACC_NOT_SERIALIZABLE);

	return class_entry;
}
