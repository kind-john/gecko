// Copyright (C) 2015 the V8 project authors. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.
/*---
es6id: 23.3.1
description: >
  The WeakMap constructor is the %WeakMap% intrinsic object and the initial
  value of the WeakMap property of the global object.
---*/

assert.sameValue(
  typeof WeakMap, 'function',
  'typeof WeakMap is "function"'
);

reportCompare(0, 0);
