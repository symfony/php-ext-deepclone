--TEST--
deepclone references const-expr closures through engine ids (PHP 8.6)
--EXTENSIONS--
deepclone
--SKIPIF--
<?php if (PHP_VERSION_ID < 80600) die('skip requires PHP 8.6'); ?>
--FILE--
<?php

#[Attribute(Attribute::TARGET_ALL | Attribute::IS_REPEATABLE)]
class CA { public array $args; public function __construct(mixed ...$args) { $this->args = $args; } }

#[CA(static function (): string { return self::SECRET; })]
class Fix {
    private const SECRET = 'class-secret';
    public const CALLBACKS = ['first' => static function (): string { return 'const-value'; }];
    #[CA(cb: [1, ['x' => static function (int $i): int { return $i * 2; }]])]
    public string $tagged = 'v';
    public ?Closure $factory = static function (): string { return 'prop-default'; };
    #[CA('not-a-closure')]
    #[CA(static function (): string { return 'repeated'; })]
    public function tagged(
        #[CA(static function (): string { return 'param-attr'; })]
        ?Closure $cb = static function (): string { return 'param-default'; },
    ): void {}
}

$rc = new ReflectionClass(Fix::class);

// ── Wire format: engine-id reference [class, id, line] ──
$c = $rc->getAttributes()[0]->getArguments()[0];
$line = (new ReflectionFunction($c))->getStartLine();
$d = deepclone_to_array($c);
var_dump($d['prepared'] === [Fix::class, 0, $line]);
var_dump($d['mask'] === 1);
$r = deepclone_from_array($d);
var_dump($r instanceof Closure, $r !== $c, $r() === 'class-secret');

// ── The emitted reference matches the engine's ──
$rf = new ReflectionFunction($c);
var_dump($d['prepared'] === [$rf->getConstExprClass(), $rf->getConstExprId(), $line]);

// ── Attribute sites: nested argument, repeated attribute, parameter attribute, parameter default ──
foreach ([
    [$rc->getProperty('tagged')->getAttributes()[0]->getArguments()['cb'][1]['x'], [3], 6],
    [$rc->getMethod('tagged')->getAttributes()[1]->getArguments()[0], [], 'repeated'],
    [$rc->getMethod('tagged')->getParameters()[0]->getAttributes()[0]->getArguments()[0], [], 'param-attr'],
    [$rc->getMethod('tagged')->getParameters()[0]->getDefaultValue(), [], 'param-default'],
] as [$c, $args, $expected]) {
    $d = deepclone_to_array($c);
    $rf = new ReflectionFunction($c);
    var_dump($d['prepared'] === [Fix::class, $rf->getConstExprId(), $rf->getStartLine()], deepclone_from_array($d)(...$args) === $expected);
}

// ── Constant values and property defaults have no engine id: site-based form ──
$d = deepclone_to_array(Fix::CALLBACKS['first']);
var_dump(count($d['prepared']) === 5, $d['prepared'][1] === 'CALLBACKS', deepclone_from_array($d)() === 'const-value');
$d = deepclone_to_array($rc->getProperty('factory')->getDefaultValue());
var_dump($d['prepared'][1] === '$factory', deepclone_from_array($d)() === 'prop-default');

// ── Site-based references written on PHP 8.5 still resolve ──
var_dump(deepclone_from_array(['classes' => '', 'objectMeta' => 0, 'prepared' => [Fix::class, '', 0, 0, $line], 'mask' => 1])() === 'class-secret');

// ── Same-line closures get distinct ids ──
#[CA(static function (): string { return 'first'; }, static function (): string { return 'second'; })]
class FixAmbiguous {}
$args = (new ReflectionClass(FixAmbiguous::class))->getAttributes()[0]->getArguments();
$d0 = deepclone_to_array($args[0]);
$d1 = deepclone_to_array($args[1]);
var_dump([$d0['prepared'][1], $d1['prepared'][1]] === [0, 1]);
var_dump(deepclone_from_array($d0)() === 'first', deepclone_from_array($d1)() === 'second');

// ── Enum case attribute gets an id, enum constant value stays site-based ──
enum FixEnum: string {
    #[CA(static function (): string { return 'enum-case-attr'; })]
    case Active = 'A';
    public const FILTER = static function (): string { return 'enum-const'; };
}
$d = deepclone_to_array((new ReflectionClassConstant(FixEnum::class, 'Active'))->getAttributes()[0]->getArguments()[0]);
var_dump(is_int($d['prepared'][1]), deepclone_from_array($d)() === 'enum-case-attr');
$d = deepclone_to_array(FixEnum::FILTER);
var_dump($d['prepared'][1] === 'FILTER', deepclone_from_array($d)() === 'enum-const');

// ── Property hooks ──
class FixHooked {
    public string $virtual {
        #[CA(static function (): string { return 'get-hook-attr'; })]
        get => 'vx';
    }
}
$c = (new ReflectionProperty(FixHooked::class, 'virtual'))->getHook(PropertyHookType::Get)->getAttributes()[0]->getArguments()[0];
$d = deepclone_to_array($c);
var_dump(is_int($d['prepared'][1]), deepclone_from_array($d)() === 'get-hook-attr');

// ── Trait method attribute: the using class declares the closure ──
trait FixTrait {
    #[CA(static function (): string { return 'trait-attr'; })]
    public function traitTagged(): void {}
}
class FixTraitUser { use FixTrait; }
$d = deepclone_to_array((new ReflectionClass(FixTraitUser::class))->getMethod('traitTagged')->getAttributes()[0]->getArguments()[0]);
var_dump(is_int($d['prepared'][1]), deepclone_from_array($d)() === 'trait-attr');

// ── Inherited declaration keeps the declaring class ──
class FixParent {
    #[CA(static function (): string { return 'parent-attr'; })]
    public function pm(): void {}
}
class FixChild extends FixParent {}
$c = (new ReflectionMethod(FixChild::class, 'pm'))->getAttributes()[0]->getArguments()[0];
$d = deepclone_to_array($c);
var_dump($d['prepared'][0] === FixParent::class, deepclone_from_array($d)() === 'parent-attr');

// ── First-class callables use the site-based reference, not an engine id:
// the engine id of an fcc resolves to a site the decode path cannot recreate,
// so they keep the declaration-site (5-element) form ──
class FixFcc {
    #[CA(self::helper(...))]
    public static function helper(): bool { return true; }
}
$d = deepclone_to_array((new ReflectionMethod(FixFcc::class, 'helper'))->getAttributes()[0]->getArguments()[0]);
var_dump($d['mask'] === 1, deepclone_from_array($d)() === true);

// ── ... but a crafted payload addressing the FCC site is rejected ──
try {
    deepclone_from_array(['classes' => '', 'objectMeta' => 0, 'prepared' => [FixFcc::class, 0, 1], 'mask' => 1]);
} catch (\ValueError $e) {
    var_dump($e->getMessage());
}

// ── Runtime closures still refuse, through the engine's own __serialize() ──
try {
    deepclone_to_array(static function () { return 'runtime'; });
} catch (\Exception $e) {
    var_dump($e->getMessage());
}

// ── Object graph survives a JSON round trip ──
$graph = (object) ['cb' => $rc->getAttributes()[0]->getArguments()[0]];
$d = json_decode(json_encode(deepclone_to_array($graph)), true);
var_dump((deepclone_from_array($d)->cb)() === 'class-secret');

// ── allowed_classes gating, both directions ──
try {
    deepclone_to_array($rc->getAttributes()[0]->getArguments()[0], []);
} catch (\ValueError $e) {
    var_dump($e->getMessage());
}
$d = deepclone_to_array($rc->getAttributes()[0]->getArguments()[0], ['Closure']);
try {
    deepclone_from_array($d, []);
} catch (\ValueError $e) {
    var_dump($e->getMessage());
}
try {
    deepclone_from_array($d, ['Closure']);
} catch (\ValueError $e) {
    var_dump($e->getMessage());
}
var_dump(deepclone_from_array($d, ['Closure', 'Fix'])() === 'class-secret');

// ── Stale payload ──
$d = deepclone_to_array($rc->getAttributes()[0]->getArguments()[0]);
$d['prepared'][2]++;
try {
    deepclone_from_array($d);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'stale payload, const-expr-closure moved from line'));
}

// ── Unknown id, unknown class ──
try {
    deepclone_from_array(['classes' => '', 'objectMeta' => 0, 'prepared' => [Fix::class, 999, $line], 'mask' => 1]);
} catch (\ValueError $e) {
    var_dump($e->getMessage());
}
try {
    deepclone_from_array(['classes' => '', 'objectMeta' => 0, 'prepared' => ['No\Such\ClassAtAll', 0, 1], 'mask' => 1]);
} catch (\ValueError $e) {
    var_dump($e->getMessage());
}
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
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
string(100) "deepclone_from_array(): malformed payload, const-expr-closure references a first-class callable site"
string(41) "Serialization of 'Closure' is not allowed"
bool(true)
string(52) "deepclone_to_array(): class "Closure" is not allowed"
string(54) "deepclone_from_array(): class "Closure" is not allowed"
string(50) "deepclone_from_array(): class "Fix" is not allowed"
bool(true)
bool(true)
string(110) "deepclone_from_array(): malformed payload, const-expr-closure references unknown closure id 999 in class "Fix""
string(107) "deepclone_from_array(): malformed payload, const-expr-closure references unknown class "No\Such\ClassAtAll""
