--TEST--
deepclone references closures declared in constant expressions (PHP 8.5)
--EXTENSIONS--
deepclone
--SKIPIF--
<?php if (PHP_VERSION_ID < 80500) die('skip requires PHP 8.5'); ?>
--FILE--
<?php

#[Attribute(Attribute::TARGET_ALL | Attribute::IS_REPEATABLE)]
class CA { public array $args; public function __construct(mixed ...$args) { $this->args = $args; } }

#[CA(static function (): string { return self::SECRET; })]
class Fix {
    private const SECRET = 'class-secret';
    public const CALLBACKS = ['first' => static function (): string { return 'const-value'; }];
    #[CA(
        static function (): string { return 'multi-0'; },
        static function (): string { return 'multi-1'; },
    )]
    public const TAGGED = 1;
    #[CA(cb: [1, ['x' => static function (int $i): int { return $i * 2; }]])]
    public string $tagged = 'v';
    public ?Closure $factory = static function (): string { return 'prop-default'; };
    public static ?Closure $staticFactory = static function (): string { return 'static-prop-default'; };
    #[CA('not-a-closure')]
    #[CA(static function (): string { return 'repeated'; })]
    public function tagged(
        #[CA(static function (): string { return 'param-attr'; })]
        ?Closure $cb = static function (): string { return 'param-default'; },
    ): void {}
}

enum FixEnum: string {
    #[CA(static function (): string { return 'enum-case-attr'; })]
    case Active = 'A';
    public const FILTER = static function (): string { return 'enum-const'; };
}

#[CA(static function (): string { return 'first'; }, static function (): string { return 'second'; })]
class FixAmbiguous {}

class FixPromoted {
    public function __construct(
        #[CA(static function (): string { return 'promoted'; })]
        public int $promoted = 1,
    ) {}
}

trait FixTrait {
    #[CA(static function (): string { return 'trait-attr'; })]
    public function traitTagged(): void {}
}
class FixTraitUser { use FixTrait; }

$rc = new ReflectionClass(Fix::class);

// ── Wire format: class attribute ──
$c = $rc->getAttributes()[0]->getArguments()[0];
// The stored line is relative to the declaring class.
$line = (new ReflectionFunction($c))->getStartLine() - $rc->getStartLine();
$d = deepclone_to_array($c);
var_dump($d['prepared'] === [Fix::class, '@0', $line]);
var_dump($d['mask'] === 1);
$r = deepclone_from_array($d);
var_dump($r instanceof Closure, $r !== $c, $r() === 'class-secret');

// ── Closure nested in an attribute argument array ──
$c = $rc->getProperty('tagged')->getAttributes()[0]->getArguments()['cb'][1]['x'];
$d = deepclone_to_array($c);
var_dump($d['prepared'][1] === '$tagged@0');
var_dump(deepclone_from_array($d)(3) === 6);

// ── Several closures in one attribute ──
$args = (new ReflectionClassConstant(Fix::class, 'TAGGED'))->getAttributes()[0]->getArguments();
foreach (['multi-0', 'multi-1'] as $i => $expected) {
    $d = deepclone_to_array($args[$i]);
    var_dump($d['prepared'][1] === 'TAGGED@'.$i, deepclone_from_array($d)() === $expected);
}

// ── Repeated attribute, parameter attribute, parameter default ──
$m = $rc->getMethod('tagged');
var_dump(deepclone_from_array(deepclone_to_array($m->getAttributes()[1]->getArguments()[0]))() === 'repeated');
var_dump(deepclone_from_array(deepclone_to_array($m->getParameters()[0]->getAttributes()[0]->getArguments()[0]))() === 'param-attr');
var_dump(deepclone_from_array(deepclone_to_array($m->getParameters()[0]->getDefaultValue()))() === 'param-default');

// ── Property defaults and constant values ──
var_dump(deepclone_from_array(deepclone_to_array($rc->getProperty('factory')->getDefaultValue()))() === 'prop-default');
var_dump(deepclone_from_array(deepclone_to_array($rc->getProperty('staticFactory')->getDefaultValue()))() === 'static-prop-default');
var_dump(deepclone_from_array(deepclone_to_array(Fix::CALLBACKS['first']))() === 'const-value');

// ── Enums ──
var_dump(deepclone_from_array(deepclone_to_array((new ReflectionClassConstant(FixEnum::class, 'Active'))->getAttributes()[0]->getArguments()[0]))() === 'enum-case-attr');
var_dump(deepclone_from_array(deepclone_to_array(FixEnum::FILTER))() === 'enum-const');

// ── Trait method attribute ──
var_dump(deepclone_from_array(deepclone_to_array((new ReflectionClass(FixTraitUser::class))->getMethod('traitTagged')->getAttributes()[0]->getArguments()[0]))() === 'trait-attr');

// ── Promoted constructor property: parameter surface is canonical; the
//    engine names the property surface instead, both resolve everywhere ──
$c = (new ReflectionProperty(FixPromoted::class, 'promoted'))->getAttributes()[0]->getArguments()[0];
$d = deepclone_to_array($c);
var_dump($d['prepared'][1] === (PHP_VERSION_ID >= 80600 ? '$promoted@0' : '__construct()@0'), deepclone_from_array($d)() === 'promoted');

// ── Same-line closures are told apart by op_array identity ──
$args = (new ReflectionClass(FixAmbiguous::class))->getAttributes()[0]->getArguments();
var_dump(deepclone_from_array(deepclone_to_array($args[0]))() === 'first');
var_dump(deepclone_from_array(deepclone_to_array($args[1]))() === 'second');

// ── Runtime closures still refuse ──
try {
    deepclone_to_array(static function () { return 'runtime'; });
} catch (\DeepClone\NotInstantiableException $e) {
    var_dump($e->getMessage());
}

// ── Object graph survives a JSON round trip ──
$graph = (object) ['cb' => $rc->getAttributes()[0]->getArguments()[0]];
$d = json_decode(json_encode(deepclone_to_array($graph)), true);
var_dump((deepclone_from_array($d)->cb)() === 'class-secret');

// ── Property hooks ──
class FixHooked {
    public string $virtual {
        #[CA(static function (): string { return 'get-hook-attr'; })]
        get => 'vx';
    }
    public string $stored = 'init' {
        set (#[CA(static function (): string { return 'set-hook-param-attr'; })] string $value) { $this->stored = $value; }
    }
}
$c = (new ReflectionProperty(FixHooked::class, 'virtual'))->getHook(PropertyHookType::Get)->getAttributes()[0]->getArguments()[0];
$d = deepclone_to_array($c);
var_dump($d['prepared'][1] === '$virtual::get()@0', deepclone_from_array($d)() === 'get-hook-attr');
$c = (new ReflectionProperty(FixHooked::class, 'stored'))->getHook(PropertyHookType::Set)->getParameters()[0]->getAttributes()[0]->getArguments()[0];
$d = deepclone_to_array($c);
var_dump($d['prepared'][1] === '$stored::set()@0', deepclone_from_array($d)() === 'set-hook-param-attr');

// ── Factory constant: outer round-trips, inner runtime closure refuses ──
class FixFactory { public const FACTORY = static function (): Closure { return static function (): string { return 'inner'; }; }; }
var_dump(deepclone_from_array(deepclone_to_array(FixFactory::FACTORY))()() === 'inner');
try { deepclone_to_array((FixFactory::FACTORY)()); } catch (\DeepClone\NotInstantiableException $e) { var_dump($e->getMessage()); }

// ── Runtime closure sharing the attribute literal line: op_array identity keeps them apart ──
class FixCollide { #[CA(static function (): string { return 'collide-const-expr'; })] public static function make(): Closure { return static function (): string { return 'collide-runtime'; }; } }
var_dump(deepclone_from_array(deepclone_to_array((new ReflectionMethod(FixCollide::class, 'make'))->getAttributes()[0]->getArguments()[0]))() === 'collide-const-expr');
try { deepclone_to_array(FixCollide::make()); } catch (\DeepClone\NotInstantiableException $e) { var_dump($e->getMessage()); }

// ── One literal reachable through several surfaces ──
#[CA(new FixDual())]
class FixDual { public function __construct(public ?Closure $cb = static function (): string { return 'dual-surface'; }) {} }
var_dump(deepclone_from_array(deepclone_to_array((new FixDual())->cb))() === 'dual-surface');

// ── Aliased trait method ──
trait TFixAlias { #[CA(static function (): string { return 'alias-trait-attr'; })] public function tm(): void {} }
class FixAliased { use TFixAlias { tm as tmAlias; } }
var_dump(deepclone_from_array(deepclone_to_array((new ReflectionMethod(FixAliased::class, 'tm'))->getAttributes()[0]->getArguments()[0]))() === 'alias-trait-attr');

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
    var_dump(str_contains($e->getMessage(), 'stale payload, const-expr-closure moved from class-relative line'));
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
string(35) "Type "Closure" is not instantiable."
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
string(35) "Type "Closure" is not instantiable."
bool(true)
string(35) "Type "Closure" is not instantiable."
bool(true)
bool(true)
string(52) "deepclone_to_array(): class "Closure" is not allowed"
string(54) "deepclone_from_array(): class "Closure" is not allowed"
string(50) "deepclone_from_array(): class "Fix" is not allowed"
bool(true)
bool(true)
