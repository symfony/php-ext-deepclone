--TEST--
deepclone round-trips a Random\Randomizer that does not outlive its payload
--EXTENSIONS--
deepclone
--SKIPIF--
<?php if (PHP_VERSION_ID < 80200) die('skip requires PHP 8.2'); ?>
--FILE--
<?php

// Before PHP 8.3, Randomizer::__serialize() returns its raw property table,
// whose "engine" slot is an IS_INDIRECT pointer into the object. The payload
// must not retain it once the source Randomizer is released.
$expected = (new Random\Randomizer(new Random\Engine\Mt19937(42)))->getInt(1, PHP_INT_MAX);

$d = deepclone_to_array(new Random\Randomizer(new Random\Engine\Mt19937(42)));
gc_collect_cycles();
$clone = deepclone_from_array($d);
var_dump($clone instanceof Random\Randomizer);
var_dump($expected === $clone->getInt(1, PHP_INT_MAX));

// Same with the Randomizer nested in a temporary object graph
$g = deepclone_from_array(deepclone_to_array((object) ['list' => [(object) ['r' => new Random\Randomizer(new Random\Engine\Mt19937(9))]]]));
var_dump($g->list[0]->r instanceof Random\Randomizer);

// And behind a shared identity
$r = new Random\Randomizer(new Random\Engine\Mt19937(5));
$c = deepclone_from_array(deepclone_to_array([$r, $r]));
var_dump($c[0] === $c[1]);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
