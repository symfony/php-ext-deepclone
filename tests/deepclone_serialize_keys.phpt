--TEST--
deepclone_to_array() handles the keys returned by __serialize() without __unserialize() like unserialize()
--EXTENSIONS--
deepclone
--FILE--
<?php

#[AllowDynamicProperties]
class Serializing
{
    private $b = 'default';
    protected $p = 'default';
    public $pub = 'default';
    public $data = [];

    public function __serialize(): array { return $this->data; }
}

function state(object $o): string
{
    $state = [];
    foreach ((array) $o as $k => $v) {
        if ('data' !== $k) {
            $state[] = strtr($k, ["\0" => '~']).'='.json_encode($v);
        }
    }

    return implode(', ', $state);
}

function check(string $label, array $data): void
{
    $o = new Serializing();
    $o->data = $data;
    $payload = deepclone_to_array($o);
    echo $label, ': ', json_encode($payload['properties'] ?? null), "\n";
    echo '  ', state(deepclone_from_array($payload)), "\n";
    $native = state(unserialize(serialize($o)));
    if ($native !== $state = state(deepclone_from_array($payload))) {
        echo '  unserialize(): ', $native, "\n";
    }
}

// The last key naming a property wins, even with its default value
check('bare then mangled private at its default', ['b' => 'bare', "\0Serializing\0b" => 'default']);
check('bare then mangled protected at its default', ['p' => 'bare', "\0*\0p" => 'default']);
check('mangled then bare private at its default', ["\0Serializing\0b" => 'mangled', 'b' => 'default']);

// Undeclared names
check('integer key', [7 => 'seven']);
check('undeclared protected name', ["\0*\0undeclared" => 1]);
?>
--EXPECT--
bare then mangled private at its default: {"Serializing":{"b":["default"]}}
  ~Serializing~b="default", ~*~p="default", pub="default"
bare then mangled protected at its default: {"Serializing":{"p":["default"]}}
  ~Serializing~b="default", ~*~p="default", pub="default"
mangled then bare private at its default: {"Serializing":{"b":["default"]}}
  ~Serializing~b="default", ~*~p="default", pub="default"
integer key: {"stdClass":{"7":["seven"]}}
  ~Serializing~b="default", ~*~p="default", pub="default", 7="seven"
undeclared protected name: {"stdClass":{"undeclared":[1]}}
  ~Serializing~b="default", ~*~p="default", pub="default", undeclared=1
  unserialize(): ~Serializing~b="default", ~*~p="default", pub="default", ~*~undeclared=1
