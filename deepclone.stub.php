<?php

/**
 * @generate-class-entries
 */

namespace DeepClone {
    class NotInstantiableException extends \InvalidArgumentException {}

    class ClassNotFoundException extends \InvalidArgumentException {}
}

namespace {
    function deepclone_to_array(mixed $value, ?array $allowed_classes = null): array {}

    function deepclone_from_array(array $data, ?array $allowed_classes = null): mixed {}

    function deepclone_hydrate(object|string $object_or_class, array $scoped_vars = [], array $mangled_vars = []): object {}
}
