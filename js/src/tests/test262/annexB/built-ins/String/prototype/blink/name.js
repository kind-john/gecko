// Copyright (C) 2015 André Bargull. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
es6id: B.2.3.4
description: >
  String.prototype.blink.name is "blink".
info: >
  String.prototype.blink ( )

  17 ECMAScript Standard Built-in Objects:
    Every built-in Function object, including constructors, that is not
    identified as an anonymous function has a name property whose value
    is a String.

    Unless otherwise specified, the name property of a built-in Function
    object, if it exists, has the attributes { [[Writable]]: false,
    [[Enumerable]]: false, [[Configurable]]: true }.
includes: [propertyHelper.js]
---*/

assert.sameValue(String.prototype.blink.name, "blink");

verifyNotEnumerable(String.prototype.blink, "name");
verifyNotWritable(String.prototype.blink, "name");
verifyConfigurable(String.prototype.blink, "name");

reportCompare(0, 0);
