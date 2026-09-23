--TEST--
Rule A: final internal classes with create_object but no serialization API round-trip via probe
--EXTENSIONS--
deepclone
tidy
--SKIPIF--
<?php
if (PHP_VERSION_ID >= 80600) {
    die('skip tidyNode is not serializable as of PHP 8.6');
}
?>
--FILE--
<?php

// tidyNode is a final internal class with a C-level create_object handler
// and no __serialize/__unserialize/__sleep/__wakeup.

$tidy = new tidy();
$tidy->parseString('<p><b>hello</b></p>', [], 'utf8');
$b = $tidy->body()->child[0]->child[0]; // <b> node

$clone = deepclone_from_array(deepclone_to_array($b));

var_dump($clone instanceof tidyNode);
var_dump($clone !== $b);
var_dump($clone->name === $b->name);
var_dump($clone->value === $b->value);

echo "Done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
Done
