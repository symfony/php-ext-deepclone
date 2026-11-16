--TEST--
deepclone_hydrate() with DEEPCLONE_HYDRATE_NO_LAZY_INIT keeps nothing from the user code that runs when the request shuts down
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80400) die('skip lazy objects require PHP 8.4+');
?>
--FILE--
<?php

class Foo
{
    public int $a = 0;
    public int $b = 0;
}

function hydrate(): int
{
    $ghost = (new ReflectionClass(Foo::class))->newLazyGhost(function () {});
    deepclone_hydrate($ghost, ['a' => 1], DEEPCLONE_HYDRATE_NO_LAZY_INIT);

    return $ghost->a;
}

class Wrapper
{
    public $context;

    public function stream_open(): bool { return true; }
    public function stream_close(): void { echo 'after the request: ', hydrate(), "\n"; }
}

stream_wrapper_register('late', Wrapper::class);

echo 'during the request: ', hydrate(), "\n";

// PHP closes the stream after the extensions were told that the request ends
$GLOBALS['stream'] = fopen('late://x', 'r');

?>
--EXPECT--
during the request: 1
after the request: 1
