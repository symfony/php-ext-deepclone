<?php

/**
 * @generate-class-entries
 */

namespace DeepClone {
    class NotInstantiableException extends \InvalidArgumentException {}

    class ClassNotFoundException extends \InvalidArgumentException {}
}

namespace {
    /**
     * @var int
     * @cvalue DEEPCLONE_HYDRATE_CALL_HOOKS
     */
    const DEEPCLONE_HYDRATE_CALL_HOOKS = UNKNOWN;

    /**
     * @var int
     * @cvalue DEEPCLONE_HYDRATE_NO_LAZY_INIT
     */
    const DEEPCLONE_HYDRATE_NO_LAZY_INIT = UNKNOWN;

    /**
     * @var int
     * @cvalue DEEPCLONE_HYDRATE_MANGLED_VARS
     */
    const DEEPCLONE_HYDRATE_MANGLED_VARS = UNKNOWN;

    function deepclone_to_array(mixed $value, ?array $allowed_classes = null): array {}

    function deepclone_from_array(array $data, ?array $allowed_classes = null): mixed {}

    function deepclone_hydrate(object|string $object_or_class, array $vars = [], int $flags = 0): object {}
}
