--TEST--
deepclone_to_array() rejects tidyNode, not serializable as of PHP 8.6
--EXTENSIONS--
deepclone
tidy
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80600) {
    die('skip tidyNode is serializable before PHP 8.6');
}
?>
--FILE--
<?php

$tidy = new tidy();
$tidy->parseString('<p><b>hello</b></p>', [], 'utf8');

try {
    deepclone_to_array($tidy->body()->child[0]->child[0]);
} catch (DeepClone\NotInstantiableException $e) {
    echo $e->getMessage(), "\n";
}
?>
--EXPECT--
Type "tidyNode" is not instantiable.
