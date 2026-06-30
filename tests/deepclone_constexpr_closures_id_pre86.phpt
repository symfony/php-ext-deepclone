--TEST--
deepclone_from_array() rejects engine-id const-expr-closure payloads before PHP 8.6
--EXTENSIONS--
deepclone
--SKIPIF--
<?php if (PHP_VERSION_ID >= 80600) die('skip PHP < 8.6 only'); ?>
--FILE--
<?php

class Fix {}

try {
    deepclone_from_array(['classes' => '', 'objectMeta' => 0, 'prepared' => [Fix::class, 0, 1], 'mask' => 1]);
} catch (\ValueError $e) {
    echo $e->getMessage(), "\n";
}
?>
--EXPECTF--
deepclone_from_array(): const-expr-closure payload was created on PHP 8.6 or later and cannot be resolved on PHP %s
