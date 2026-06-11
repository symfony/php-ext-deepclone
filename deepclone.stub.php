<?php

/**
 * @generate-class-entries
 */

namespace DeepClone {
    class NotInstantiableException extends \InvalidArgumentException {}

    class ClassNotFoundException extends \InvalidArgumentException {}

    /**
     * Shared hydration state behind the lazy ghosts deepclone_from_array()
     * creates for closure-bearing object nodes: retains the payload, the
     * object table, the shared references and the allow-list until every
     * ghost has been initialized. Instances are created internally only;
     * the ghosts' initializer (as returned by
     * ReflectionClass::getLazyInitializer()) is a Closure bound to one.
     *
     * @strict-properties
     * @not-serializable
     */
    final class HydrationContext
    {
        private function __construct() {}

        private function hydrate(object $object): void {}
    }
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
     * @cvalue DEEPCLONE_HYDRATE_PRESERVE_REFS
     */
    const DEEPCLONE_HYDRATE_PRESERVE_REFS = UNKNOWN;

    function deepclone_to_array(mixed $value, ?array $allowed_classes = null, bool $allow_named_closures = false): array {}

    function deepclone_from_array(array $data, ?array $allowed_classes = null, bool $allow_named_closures = false): mixed {}

    function deepclone_hydrate(object|string $object_or_class, array $vars = [], int $flags = 0): object {}
}
