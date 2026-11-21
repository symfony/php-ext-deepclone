--TEST--
deepclone_hydrate() calls __set() with the declaring class visible in debug_backtrace()
--EXTENSIONS--
deepclone
--FILE--
<?php

class GhostState
{
    protected $cities;
    protected $id;
}

/* Mirrors Symfony VarExporter's LazyObjectRegistry::getScopeForWrite(): a
 * missing PHP frame with a class means the write accessor cannot be scoped
 * to the class that declares the protected property. */
function getScopeForWrite(): string
{
    $frame = debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS, 3)[2] ?? [];
    return $frame['class'] ?? Closure::class;
}

class Ghost extends GhostState
{
    public static array $setCalls = [];
    public static array $scopes = [];

    public function __construct()
    {
        unset($this->cities, $this->id);
    }

    public function __set(string $name, mixed $value): void
    {
        self::$setCalls[] = $name;
        $scope = getScopeForWrite();
        self::$scopes[] = $scope;

        $setter = Closure::bind(
            static function (object $o, string $n, mixed $v): void { $o->$n = $v; },
            null,
            Closure::class === $scope ? null : $scope
        );
        $setter($this, $name, $value);
    }

    public function values(): array
    {
        return [$this->cities, $this->id];
    }
}

$values = (array) (new ReflectionClass(Ghost::class))->newInstanceWithoutConstructor();
$values["\0*\0cities"] = ['Paris'];
$values["\0*\0id"] = 42;

foreach ([0, DEEPCLONE_HYDRATE_CALL_HOOKS] as $flags) {
    Ghost::$setCalls = Ghost::$scopes = [];
    deepclone_hydrate($ghost = new Ghost(), $values, $flags);
    var_dump($flags, Ghost::$setCalls, Ghost::$scopes, $ghost->values());
}

?>
--EXPECT--
int(0)
array(2) {
  [0]=>
  string(6) "cities"
  [1]=>
  string(2) "id"
}
array(2) {
  [0]=>
  string(10) "GhostState"
  [1]=>
  string(10) "GhostState"
}
array(2) {
  [0]=>
  array(1) {
    [0]=>
    string(5) "Paris"
  }
  [1]=>
  int(42)
}
int(1)
array(2) {
  [0]=>
  string(6) "cities"
  [1]=>
  string(2) "id"
}
array(2) {
  [0]=>
  string(10) "GhostState"
  [1]=>
  string(10) "GhostState"
}
array(2) {
  [0]=>
  array(1) {
    [0]=>
    string(5) "Paris"
  }
  [1]=>
  int(42)
}
