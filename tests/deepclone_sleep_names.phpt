--TEST--
deepclone_to_array() resolves the names returned by __sleep() like serialize()
--EXTENSIONS--
deepclone
--FILE--
<?php

// As for serialize(), a mangled name selects the property it mangles, and a
// bare name the public or dynamic property, the protected property or the
// private property of the object's class by that name: never a private
// property of a parent class.

class Base
{
    private $secret = 'default';
    protected $prot = 'default';
    public $pub = 'default';

    public function setBase($v): void
    {
        $this->secret = $this->prot = $this->pub = $v;
    }
}

class ParentPrivateBare extends Base
{
    public function __sleep(): array { return ['secret', 'pub']; }
}

class ParentPrivateMangled extends Base
{
    public function __sleep(): array { return ["\0Base\0secret", 'pub']; }
}

class ProtectedBare extends Base
{
    public function __sleep(): array { return ['prot']; }
}

class ProtectedMangled extends Base
{
    public function __sleep(): array { return ["\0*\0prot"]; }
}

class SamePrivate extends Base
{
    private $secret = 'default';

    public function setChild($v): void { $this->secret = $v; }

    public function __sleep(): array { return ['secret']; }
}

// Inherits the __sleep() of SamePrivate but declares no private $secret
class SamePrivateChild extends SamePrivate
{
}

class SamePrivateParentMangled extends Base
{
    private $secret = 'default';

    public function setChild($v): void { $this->secret = $v; }

    public function __sleep(): array { return ["\0Base\0secret"]; }
}

class SamePrivateBoth extends Base
{
    private $secret = 'default';

    public function setChild($v): void { $this->secret = $v; }

    public function __sleep(): array { return ['secret', "\0Base\0secret"]; }
}

class ChildPublic extends Base
{
    public $secret = 'default';

    public function __sleep(): array { return ['secret']; }
}

class ChildProtected extends Base
{
    protected $secret = 'default';

    public function setChild($v): void { $this->secret = $v; }

    public function __sleep(): array { return ['secret']; }
}

#[AllowDynamicProperties]
class Dynamic extends Base
{
    public function __sleep(): array { return ['secret', 'dyn', 'missing']; }
}

class OwnPrivate
{
    private $a = 'default';
    private $b = 'default';

    public function set($v): void { $this->a = $this->b = $v; }

    public function __sleep(): array { return ['a', "\0OwnPrivate\0b"]; }
}

// __sleep() declared by the class of the private property, called on a child
class SleepingParent
{
    private $secret = 'default';

    public function set($v): void { $this->secret = $v; }

    public function __sleep(): array { return ['secret']; }
}

class SleepingParentChild extends SleepingParent
{
}

// __unserialize() gets the properties __sleep() selects
class Unserializing extends Base
{
    private $own = 'default';
    public $keys;

    public function setOwn($v): void { $this->own = $v; }

    public function __sleep(): array { return ['pub', 'secret', "\0*\0prot", 'own']; }

    public function __unserialize(array $data): void
    {
        $keys = array_keys($data);
        sort($keys);
        $this->keys = str_replace("\0", '~', implode(',', $keys));
    }
}

function check(string $label, object $o): void
{
    $export = static function () use ($o) {
        $notices = [];
        set_error_handler(static function ($type, $msg) use (&$notices) {
            $notices[] = $msg;

            return true;
        });
        try {
            return [deepclone_to_array($o), $notices];
        } finally {
            restore_error_handler();
        }
    };

    [$data, $notices] = $export();
    // Building the property table moves the export to another code path
    foreach ($o as $v) {
    }
    $samePaths = [$data, $notices] === $export();
    $sameAsNative = (array) deepclone_from_array($data) == (array) @unserialize(@serialize($o));

    echo $label, ': ', json_encode($data['properties'] ?? $data['states'] ?? null);
    echo $samePaths ? '' : ' (differs once the property table is built)';
    echo $sameAsNative ? '' : ' (differs from serialize())', "\n";
    foreach ($notices as $notice) {
        echo '  ', $notice, "\n";
    }
}

$o = new ParentPrivateBare();
$o->setBase('changed');
check('parent private, bare', $o);

$o = new ParentPrivateMangled();
$o->setBase('changed');
check('parent private, mangled', $o);

$o = new ProtectedBare();
$o->setBase('changed');
check('protected, bare', $o);

$o = new ProtectedMangled();
$o->setBase('changed');
check('protected, mangled', $o);

$o = new SamePrivate();
$o->setBase('parent');
$o->setChild('child');
check('same private, bare', $o);

$o = new SamePrivateChild();
$o->setBase('parent');
$o->setChild('child');
check('same private from a child class, bare', $o);

$o = new SamePrivateParentMangled();
$o->setBase('parent');
$o->setChild('child');
check('same private, parent mangled', $o);

$o = new SamePrivateBoth();
$o->setBase('parent');
$o->setChild('child');
check('same private, bare and parent mangled', $o);

$o = new ChildPublic();
$o->setBase('parent');
$o->secret = 'child';
check('child public, bare', $o);

$o = new ChildProtected();
$o->setBase('parent');
$o->setChild('child');
check('child protected, bare', $o);

$o = new Dynamic();
$o->setBase('parent');
$o->dyn = 'dynamic';
check('dynamic', $o);

$o = new Dynamic();
$o->setBase('parent');
$o->secret = 'dynamic';
$o->dyn = 'dynamic';
check('dynamic named like a parent private', $o);

$o = new OwnPrivate();
$o->set('changed');
check('own private, bare and mangled', $o);

$o = new SleepingParent();
$o->set('changed');
check('private of the class declaring __sleep()', $o);

$o = new SleepingParentChild();
$o->set('changed');
check('private of the parent declaring __sleep()', $o);

$o = new Unserializing();
$o->setBase('changed');
$o->setOwn('changed');
check('__unserialize()', $o);
var_dump(deepclone_from_array(@deepclone_to_array($o))->keys);
?>
--EXPECT--
parent private, bare: {"stdClass":{"pub":["changed"]}}
  deepclone_to_array(): serialize(): "secret" returned as member variable from __sleep() but does not exist
parent private, mangled: {"Base":{"secret":["changed"]},"stdClass":{"pub":["changed"]}}
protected, bare: {"Base":{"prot":["changed"]}}
protected, mangled: {"Base":{"prot":["changed"]}}
same private, bare: {"SamePrivate":{"secret":["child"]}}
same private from a child class, bare: null
  deepclone_to_array(): serialize(): "secret" returned as member variable from __sleep() but does not exist
same private, parent mangled: {"Base":{"secret":["parent"]}}
same private, bare and parent mangled: {"Base":{"secret":["parent"]},"SamePrivateBoth":{"secret":["child"]}}
child public, bare: {"stdClass":{"secret":["child"]}}
child protected, bare: {"ChildProtected":{"secret":["child"]}}
dynamic: {"stdClass":{"dyn":["dynamic"]}}
  deepclone_to_array(): serialize(): "secret" returned as member variable from __sleep() but does not exist
  deepclone_to_array(): serialize(): "missing" returned as member variable from __sleep() but does not exist
dynamic named like a parent private: {"Base":{"secret":["dynamic"]},"stdClass":{"dyn":["dynamic"]}}
  deepclone_to_array(): serialize(): "missing" returned as member variable from __sleep() but does not exist
own private, bare and mangled: {"OwnPrivate":{"a":["changed"],"b":["changed"]}}
private of the class declaring __sleep(): {"SleepingParent":{"secret":["changed"]}}
private of the parent declaring __sleep(): null
  deepclone_to_array(): serialize(): "secret" returned as member variable from __sleep() but does not exist
__unserialize(): {"1":[0,{"\u0000*\u0000prot":"changed","pub":"changed","\u0000Unserializing\u0000own":"changed"}]}
  deepclone_to_array(): serialize(): "secret" returned as member variable from __sleep() but does not exist
string(30) "~*~prot,~Unserializing~own,pub"
