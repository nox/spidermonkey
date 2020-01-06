// Copyright (C) 2016 the V8 project authors. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.
/*---
esid: sec-runtime-semantics-classdefinitionevaluation
description: >
  Attempting to call `super()` in a null-extending class throws a TypeError,
  because %FunctionPrototype% cannot be called as constructor function.
info: |
  Runtime Semantics: ClassDefinitionEvaluation

  [...]
  5. If ClassHeritageopt is not present, then
     [...]
  6. Else,
     [...]
     b. Let superclass be the result of evaluating ClassHeritage.
     [...]
     e. If superclass is null, then
        [...]
        ii. Let constructorParent be the intrinsic object %FunctionPrototype%.
  [...]
  15. Let constructorInfo be the result of performing DefineMethod for constructor with arguments proto and constructorParent as the optional functionPrototype argument.
  [...]

  12.3.5.1 Runtime Semantics: Evaluation

  SuperCall : super Arguments

  [...]
  3. Let func be ? GetSuperConstructor().
  4. Let argList be ArgumentListEvaluation of Arguments.
  [...]

  12.3.5.2 Runtime Semantics: GetSuperConstructor ( )

  [...]
  5. Let superConstructor be ! activeFunction.[[GetPrototypeOf]]().
  6. If IsConstructor(superConstructor) is false, throw a TypeError exception.
  [...]
---*/

var unreachable = 0;
var reachable = 0;

class C extends null {
  constructor() {
    reachable += 1;
    super(unreachable += 1);
    unreachable += 1;
  }
}

assert.throws(TypeError, function() {
  new C();
});

assert.sameValue(reachable, 1);
assert.sameValue(unreachable, 0);

reportCompare(0, 0);
