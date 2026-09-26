--TEST--
Classes of dom, xsl, mysqli and soap round-trip like with unserialize()
--EXTENSIONS--
deepclone
dom
--FILE--
<?php

function state(object $o): array
{
    return [$o::class, (array) $o, $o instanceof Countable ? count($o) : null];
}

$doc = new DOMDocument();
$doc->loadXML('<a x="1"><b/><b/></a>');
$values = [
    $doc->getElementsByTagName('b'),
    $doc->documentElement->attributes,
    new DOMImplementation(),
];

// Output only covers dom, which may be the only one loaded
$optional = [];
if (class_exists(Dom\XMLDocument::class)) {
    $xml = Dom\XMLDocument::createFromString('<a x="1"><b/><b/></a>');
    $optional[] = $xml->getElementsByTagName('b');
    $optional[] = $xml->documentElement->childNodes;
    $optional[] = $xml->documentElement->attributes;
}
if (class_exists(XSLTProcessor::class)) {
    $optional[] = new XSLTProcessor();
}
if (class_exists(mysqli::class)) {
    $optional[] = (new ReflectionClass(mysqli::class))->newInstanceWithoutConstructor();
}
if (class_exists(SoapServer::class)) {
    $optional[] = new SoapServer(null, ['uri' => 'urn:deepclone']);
}

foreach ([...$values, ...$optional] as $i => $o) {
    $clone = deepclone_from_array(deepclone_to_array($o));
    $ok = state($clone) == state(unserialize(serialize($o)));
    if (isset($values[$i])) {
        echo $o::class, ': ', var_export($ok, true), "\n";
    } elseif (!$ok) {
        echo $o::class, ": false\n";
    }
}

foreach ([DOMNodeList::class, DOMNamedNodeMap::class, DOMImplementation::class, 'XSLTProcessor', 'mysqli', 'mysqli_result', 'mysqli_stmt', 'SoapServer'] as $class) {
    if (class_exists($class)) {
        $o = deepclone_hydrate($class);
        if ($o::class !== $class) {
            echo $class, ": ", $o::class, "\n";
        }
    }
}

// Classes of other extensions are still rejected
if (class_exists(XMLWriter::class)) {
    try {
        deepclone_to_array(new XMLWriter());
        echo "XMLWriter: accepted\n";
    } catch (DeepClone\NotInstantiableException $e) {
    }
}
?>
--EXPECT--
DOMNodeList: true
DOMNamedNodeMap: true
DOMImplementation: true
