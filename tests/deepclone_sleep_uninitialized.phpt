--TEST--
deepclone_to_array() silently skips __sleep()-listed uninitialized typed properties (matches native serialize)
--EXTENSIONS--
deepclone
--FILE--
<?php

// Reproducer for symfony/symfony#63957: Doctrine\ORM\Mapping classes return
// inherited typed property names from __sleep() that may be uninitialized
// on concrete subclasses. Native serialize() omits them silently; so must we.
abstract class AssocBase {
    public string $sourceEntity;
    public string $mappedBy;
    private string $secret = 'def';
    public function __sleep(): array { return ['sourceEntity', 'mappedBy']; }
    public function setSecret(string $v): void { $this->secret = $v; }
}
class OneToMany extends AssocBase {
    public function __construct(string $src) { $this->sourceEntity = $src; }
}

$o = new OneToMany('entity');
$o->setSecret('changed');

// No notice should be raised.
$errors = [];
set_error_handler(function ($_, $msg) use (&$errors) { $errors[] = $msg; return true; });
try {
    $payload = deepclone_to_array($o);
} finally {
    restore_error_handler();
}
var_dump($errors === []);

// Roundtrip works even though parent scope is abstract.
$clone = deepclone_from_array($payload);
var_dump($clone instanceof OneToMany);
var_dump($clone->sourceEntity === 'entity');

// Unknown __sleep entries still warn (native behavior).
class WithBadSleep {
    public string $a = 'a';
    public function __sleep(): array { return ['a', 'nope']; }
}
$errors = [];
set_error_handler(function ($_, $msg) use (&$errors) { $errors[] = $msg; return true; });
try {
    deepclone_to_array(new WithBadSleep());
} finally {
    restore_error_handler();
}
var_dump(count($errors) === 1);
var_dump(str_contains($errors[0], '"nope" returned as member variable'));

echo "Done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
Done
