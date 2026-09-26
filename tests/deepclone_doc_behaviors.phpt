--TEST--
deepclone_to_array() matches Symfony VarExporter documented behaviors
--EXTENSIONS--
deepclone
--FILE--
<?php

// 1. Missing class on deepclone_from_array — must throw a
//    \DeepClone\ClassNotFoundException (an \InvalidArgumentException subclass)
//    with the bare class name as the message, so that a PHP-side wrapper
//    (e.g. Symfony's NativeDeepClonerTrait) can rewrap it into its own
//    ClassNotFoundException via instanceof.
try {
    deepclone_from_array([
        'classes' => 'NonExistentClassXyz',
        'objectMeta' => 1,
        'prepared' => 0,
        'properties' => ['NonExistentClassXyz' => ['foo' => ['bar']]],
    ]);
    echo "1. FAIL: no exception\n";
} catch (\InvalidArgumentException $e) {
    $ok = $e instanceof \DeepClone\ClassNotFoundException && str_contains($e->getMessage(), 'NonExistentClassXyz');
    echo '1. ', $ok ? 'OK' : 'FAIL', ': ', $e::class, ' msg=', $e->getMessage(), "\n";
}

// 2a. SplObjectStorage: identity preservation when an object appears both as a
//     storage key AND outside the storage.
$key = new stdClass();
$key->id = 'shared-key';
$storage = new SplObjectStorage();
$storage[$key] = 'value';
$graph = new stdClass();
$graph->storage = $storage;
$graph->keyOutside = $key;

$clone = deepclone_from_array(deepclone_to_array($graph));
$clonedKey = null;
foreach ($clone->storage as $k) {
    $clonedKey = $k;
}
echo '2a. SplObjectStorage shared key: ', ($clone->keyOutside === $clonedKey) ? 'OK' : 'FAIL', "\n";

// 2b. SplObjectStorage: identity preservation for stored values
$value = new stdClass();
$value->id = 'shared-value';
$key2 = new stdClass();
$storage2 = new SplObjectStorage();
$storage2[$key2] = $value;
$graph2 = new stdClass();
$graph2->storage = $storage2;
$graph2->valueOutside = $value;

$clone2 = deepclone_from_array(deepclone_to_array($graph2));
$clonedK = null;
foreach ($clone2->storage as $k) {
    $clonedK = $k;
}
echo '2b. SplObjectStorage shared value: ', ($clone2->valueOutside === $clone2->storage[$clonedK]) ? 'OK' : 'FAIL', "\n";

// 2c. ArrayObject: identity preservation for items inside
$shared = new stdClass();
$shared->id = 'shared';
$ao = new ArrayObject(['inside' => $shared]);
$graph3 = new stdClass();
$graph3->ao = $ao;
$graph3->outside = $shared;

$clone3 = deepclone_from_array(deepclone_to_array($graph3));
echo '2c. ArrayObject shared item: ', ($clone3->outside === $clone3->ao['inside']) ? 'OK' : 'FAIL', "\n";

// 2d. ArrayIterator: identity preservation for items inside
$shared2 = new stdClass();
$shared2->id = 'shared';
$ai = new ArrayIterator(['inside' => $shared2]);
$graph4 = new stdClass();
$graph4->ai = $ai;
$graph4->outside = $shared2;

$clone4 = deepclone_from_array(deepclone_to_array($graph4));
echo '2d. ArrayIterator shared item: ', ($clone4->outside === $clone4->ai['inside']) ? 'OK' : 'FAIL', "\n";

// 3. Reflection / IteratorIterator / RecursiveIteratorIterator / anonymous classes must
//    throw at serialize time
$nonInstantiable = [
    'ReflectionClass'           => new ReflectionClass(stdClass::class),
    'ReflectionMethod'          => new ReflectionMethod(ArrayObject::class, '__construct'),
    'ReflectionProperty'        => new ReflectionProperty(Error::class, 'message'),
    'IteratorIterator'          => new IteratorIterator(new ArrayIterator([1, 2])),
    'RecursiveIteratorIterator' => new RecursiveIteratorIterator(new RecursiveArrayIterator([[1]])),
    'anonymous class'           => new class { public int $x = 1; },
];

foreach ($nonInstantiable as $label => $value) {
    try {
        deepclone_to_array($value);
        echo "3. $label: FAIL (no exception)\n";
    } catch (\InvalidArgumentException $e) {
        // Must be a \DeepClone\NotInstantiableException with the bare class /
        // type name as the message. The class also extends \InvalidArgumentException
        // so older catch blocks targeting that base type still work.
        $ok = $e instanceof \DeepClone\NotInstantiableException;
        echo "3. $label: ", $ok ? 'OK' : 'FAIL', "\n";
    }
}

// 4. Internal classes with hidden non-property state (custom create_object,
//    no __serialize/__unserialize/__sleep/__wakeup, not Serializable) must be
//    rejected. Property restoration would silently produce a default-state
//    clone — e.g. an SplFileInfo with no path, a ZipArchive with no archive,
//    a PDO with no connection. We catch them up-front instead of returning
//    a corrupt clone. Only test classes from required extensions to keep
//    the suite portable; the same logic catches PDO, ZipArchive, mysqli_*,
//    SplFileObject, etc. when those extensions are loaded.
$hiddenState = [
    'SplFileInfo' => new SplFileInfo('/etc/hostname'),
];
foreach ($hiddenState as $label => $value) {
    try {
        deepclone_to_array($value);
        echo "4. $label: FAIL (no exception — would silently corrupt)\n";
    } catch (\DeepClone\NotInstantiableException $e) {
        echo "4. $label: OK (", $e->getMessage(), ")\n";
    }
}

// 4b. Classes that explicitly opt out via ZEND_ACC_NOT_SERIALIZABLE must also
//     be rejected. The canonical case is SensitiveParameterValue: it hides its
//     $value field from (array) cast (so secrets don't leak through casting),
//     so the C extension's property pipeline sees an empty bag and would
//     otherwise silently round-trip to a default-state instance — losing the
//     secret. The class also has the NOT_SERIALIZABLE flag set, which the
//     extension now honours.
$notSerializable = [
    'SensitiveParameterValue' => new SensitiveParameterValue('super-secret-api-key'),
];
foreach ($notSerializable as $label => $value) {
    try {
        deepclone_to_array($value);
        echo "4b. $label: FAIL (would silently lose hidden data)\n";
    } catch (\DeepClone\NotInstantiableException $e) {
        echo "4b. $label: OK (", $e->getMessage(), ")\n";
    }
}

// 5. Internal classes that DO declare a serialization API must keep working.
//    These all have __serialize/__unserialize and represent the real-world
//    happy path for the heuristic above.
$shouldRoundTrip = [
    'ArrayObject'         => new ArrayObject([1, 2, 3]),
    'SplFixedArray'       => SplFixedArray::fromArray([10, 20, 30]),
    'SplObjectStorage'    => (function () { $s = new SplObjectStorage; $s[new stdClass] = 'v'; return $s; })(),
    'DateTimeImmutable'   => new DateTimeImmutable('2026-04-08 12:00:00 UTC'),
    'DateTimeZone'        => new DateTimeZone('Europe/Paris'),
    'DateInterval'        => new DateInterval('P3DT4H'),
];
foreach ($shouldRoundTrip as $label => $value) {
    try {
        $clone = deepclone_from_array(deepclone_to_array($value));
        echo "5. $label: ", get_debug_type($clone) === get_debug_type($value) ? 'OK' : 'FAIL', "\n";
    } catch (\Throwable $e) {
        echo "5. $label: FAIL (", $e::class, ': ', $e->getMessage(), ")\n";
    }
}

// 6. Resources at the top level are rejected, same as mid-tree. Previously
//    the top-level fast path wrapped them in ['value' => $resource] and let
//    them escape the walker entirely.
try {
    deepclone_to_array(STDIN);
    echo "6. top-level resource: FAIL (no exception)\n";
} catch (\DeepClone\NotInstantiableException $e) {
    echo "6. top-level resource: OK (", $e->getMessage(), ")\n";
}

echo "Done\n";
?>
--EXPECT--
1. OK: DeepClone\ClassNotFoundException msg=Class "NonExistentClassXyz" not found.
2a. SplObjectStorage shared key: OK
2b. SplObjectStorage shared value: OK
2c. ArrayObject shared item: OK
2d. ArrayIterator shared item: OK
3. ReflectionClass: OK
3. ReflectionMethod: OK
3. ReflectionProperty: OK
3. IteratorIterator: OK
3. RecursiveIteratorIterator: OK
3. anonymous class: OK
4. SplFileInfo: OK (Type "SplFileInfo" is not instantiable.)
4b. SensitiveParameterValue: OK (Type "SensitiveParameterValue" is not instantiable.)
5. ArrayObject: OK
5. SplFixedArray: OK
5. SplObjectStorage: OK
5. DateTimeImmutable: OK
5. DateTimeZone: OK
5. DateInterval: OK
6. top-level resource: OK (Type "stream resource" is not instantiable.)
Done
