<?php

/**
 * @generate-class-entries
 */

namespace DeepClone {
    class NotInstantiableException extends \InvalidArgumentException {}

    class ClassNotFoundException extends \InvalidArgumentException {}
}

namespace {
    function deepclone_to_array(mixed $value, ?array $allowedClasses = null): array {}

    function deepclone_from_array(array $data, ?array $allowedClasses = null): mixed {}
}
