--TEST--
deepclone resolves cross-class and global first-class-callable declaring classes from the engine (PHP 8.6)
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80600) die('skip requires PHP 8.6');
if (!method_exists('ReflectionFunction', 'getConstExprClass')) die('skip requires native const-expr provenance');
?>
--FILE--
<?php

#[Attribute(Attribute::TARGET_ALL)]
class CA { public mixed $cb; public function __construct(mixed ...$a) { $this->cb = $a[0]; } }

class Target { public static function check(): bool { return true; } }

function dc_native_global(): int { return 41; }

class Decl {
    #[CA(Target::check(...))] public int $x = 0;      // cross-class first-class callable
    #[CA(strlen(...))] public int $g = 0;             // global internal function
    #[CA(dc_native_global(...))] public int $u = 0;   // global user function
}

// The engine yields the declaring class (no ReflectionAttribute capture, no
// allow_named_closures opt-in), and the closure serializes as the same
// site-based reference rooted at the declaring class -- not the target's scope.
$rp = new ReflectionClass(Decl::class);

$cross = deepclone_to_array($rp->getProperty('x')->getAttributes()[0]->getArguments()[0]);
var_dump($cross['mask'] === 1, $cross['prepared'][0] === Decl::class, deepclone_from_array($cross)() === true);

$gi = deepclone_to_array($rp->getProperty('g')->getAttributes()[0]->getArguments()[0]);
var_dump($gi['mask'] === 1, $gi['prepared'][0] === Decl::class, deepclone_from_array($gi)('hello') === 5);

$gu = deepclone_to_array($rp->getProperty('u')->getAttributes()[0]->getArguments()[0]);
var_dump($gu['mask'] === 1, $gu['prepared'][0] === Decl::class, deepclone_from_array($gu)() === 41);

echo "Done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
Done
