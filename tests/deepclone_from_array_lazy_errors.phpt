--TEST--
deepclone_from_array() lazy hydration: validation stays eager, resolution errors defer and retry
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80400) {
    die('skip native lazy objects require PHP 8.4+');
}
?>
--FILE--
<?php

class Node { public mixed $v = null; public mixed $w = null; }
class Other { public int $x = 0; }

function expectError(callable $fn): void
{
    try {
        $fn();
        echo "no error?!\n";
    } catch (Throwable $e) {
        echo get_class($e), ': ', $e->getMessage(), "\n";
    }
}

// All handcrafted payloads carry a named-closure marker on Node::$v so the
// node qualifies as a ghost and the deferred-slot index is exercised.
function ghostPayload(array $extraProps = [], array $extraResolve = []): array
{
    return [
        'classes' => 'Node',
        'objectMeta' => 1,
        'prepared' => 0,
        'properties' => array_merge_recursive(['stdClass' => ['v' => [0 => [null, 'strlen']]]], $extraProps),
        'resolve'    => array_merge_recursive(['stdClass' => ['v' => [0 => 0]]], $extraResolve),
    ];
}

// ── allowed_classes filtering stays eager ──
expectError(fn () => deepclone_from_array(ghostPayload(), ['Other'], true));

// ── Structural validation of deferred slots stays eager:
//    scope that is not a parent of the (would-be lazy) object ──
expectError(fn () => deepclone_from_array(
    ghostPayload(['Other' => ['x' => [0 => 1]]]), null, true));

//    unknown declared property on a (would-be lazy) object ──
expectError(fn () => deepclone_from_array(
    ghostPayload(['Node' => ['nope' => [0 => 1]]]), null, true));

// ── Value-level resolution errors defer to first access; the engine
//    reverts the ghost, which stays uninitialized and retries deterministically ──
$obj = deepclone_from_array(
    ghostPayload(['stdClass' => ['w' => [0 => [null, 'no_such_function_xyz']]]],
                 ['stdClass' => ['w' => [0 => 0]]]), null, true);
$rc = new ReflectionClass(Node::class);
var_dump($rc->isUninitializedLazyObject($obj));
expectError(fn () => $obj->w);
var_dump($rc->isUninitializedLazyObject($obj));
expectError(fn () => $obj->w);
var_dump($rc->isUninitializedLazyObject($obj));

// ── HydrationContext is internal-only ──
expectError(fn () => new DeepClone\HydrationContext);

$lazy = deepclone_from_array(deepclone_to_array((function () {
    $n = new Node; $n->v = strtoupper(...);
    return $n;
})(), null, true), null, true);
$init = $rc->getLazyInitializer($lazy);
var_dump($init instanceof Closure);
expectError(fn () => serialize($init));
expectError(fn () => $init(new stdClass));            // not managed by this context
expectError(fn () => (new ReflectionClass(DeepClone\HydrationContext::class))->newInstanceWithoutConstructor());

// the bound context stays locked down even when fished out of the Closure
$ctx = (new ReflectionFunction($init))->getClosureThis();
var_dump($ctx instanceof DeepClone\HydrationContext);
expectError(fn () => clone $ctx);
expectError(fn () => serialize($ctx));
expectError(fn () => $ctx->hydrate($lazy));           // private method

// the real ghost still hydrates fine afterwards
var_dump(($lazy->v)('ok'));

// ── A ghost realized behind the context's back must not be re-hydratable ──
$lazy2 = deepclone_from_array(deepclone_to_array((function () {
    $n = new Node; $n->v = strtoupper(...);
    return $n;
})(), null, true), null, true);
$init2 = $rc->getLazyInitializer($lazy2);
$rc->markLazyObjectAsInitialized($lazy2);
$lazy2->v = 'mine';
expectError(fn () => $init2($lazy2));   // realized: refuse to clobber
var_dump($lazy2->v);

?>
--EXPECTF--
ValueError: deepclone_from_array(): class "Node" is not allowed
ValueError: deepclone_from_array(): Argument #1 ($data) "properties" scope "Other" is not a parent of object id 0 (Node)
ValueError: deepclone_from_array(): Argument #1 ($data) "properties" value for "Node::nope" does not match a declared property on object id 0
bool(true)
ValueError: deepclone_from_array(): malformed payload, named-closure function or method not found
bool(true)
ValueError: deepclone_from_array(): malformed payload, named-closure function or method not found
bool(true)
Error: Call to private DeepClone\HydrationContext::__construct() from global scope
bool(true)
Exception: Serialization of 'Closure' is not allowed
ValueError: DeepClone\HydrationContext::hydrate(): Argument #1 ($object) is not an uninitialized lazy ghost of this context
ReflectionException: Class DeepClone\HydrationContext is an internal class marked as final that cannot be instantiated without invoking its constructor
bool(true)
Error: Trying to clone an uncloneable object of class DeepClone\HydrationContext
Exception: Serialization of 'DeepClone\HydrationContext' is not allowed
Error: Call to private method DeepClone\HydrationContext::hydrate() from global scope
string(2) "OK"
ValueError: DeepClone\HydrationContext::hydrate(): Argument #1 ($object) is not an uninitialized lazy ghost of this context
string(4) "mine"
