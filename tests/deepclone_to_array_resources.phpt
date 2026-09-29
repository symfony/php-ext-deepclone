--TEST--
deepclone_to_array() rejects resources, closed ones included, and names the argument of invalid allowed classes
--EXTENSIONS--
deepclone
--FILE--
<?php

$open = fopen('php://memory', 'r');
$closed = fopen('php://memory', 'r');
fclose($closed);

foreach ([
    'open resource' => $open,
    'closed resource' => $closed,
    'nested open resource' => [$open],
    'nested closed resource' => [1, [$closed]],
    'closed resource property' => (object) ['a' => $closed],
] as $label => $value) {
    try {
        deepclone_to_array($value);
        echo $label, ": no exception\n";
    } catch (DeepClone\NotInstantiableException $e) {
        echo $label, ': ', $e->getMessage(), "\n";
    }
}

foreach ([[123], ['not a class'], ['O:8:"stdClass":0:{}']] as $allowed) {
    try {
        deepclone_to_array(new stdClass(), $allowed);
    } catch (ValueError $e) {
        echo $e->getMessage(), "\n";
    }
    try {
        deepclone_from_array(['classes' => 'stdClass', 'objectMeta' => 1, 'prepared' => 0], $allowed);
    } catch (ValueError $e) {
        echo $e->getMessage(), "\n";
    }
}
?>
--EXPECT--
open resource: Type "stream resource" is not instantiable.
closed resource: Type "Unknown resource" is not instantiable.
nested open resource: Type "stream resource" is not instantiable.
nested closed resource: Type "Unknown resource" is not instantiable.
closed resource property: Type "Unknown resource" is not instantiable.
deepclone_to_array(): Argument $allowed_classes must be an array of class names, int given
deepclone_from_array(): Argument $allowed_classes must be an array of class names, int given
deepclone_to_array(): Argument $allowed_classes must be an array of class names, "not a class" given
deepclone_from_array(): Argument $allowed_classes must be an array of class names, "not a class" given
deepclone_to_array(): Argument $allowed_classes must be an array of class names, "O:8:"stdClass":0:{}" given
deepclone_from_array(): Argument $allowed_classes must be an array of class names, "O:8:"stdClass":0:{}" given
