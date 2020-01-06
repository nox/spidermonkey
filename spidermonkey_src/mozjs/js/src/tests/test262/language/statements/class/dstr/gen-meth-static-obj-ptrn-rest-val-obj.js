// This file was procedurally generated from the following sources:
// - src/dstr-binding/obj-ptrn-rest-val-obj.case
// - src/dstr-binding/default/cls-decl-gen-meth-static.template
/*---
description: Rest object contains just unextracted data (static class expression generator method)
esid: sec-runtime-semantics-bindingclassdeclarationevaluation
features: [object-rest, generators, destructuring-binding]
flags: [generated]
includes: [propertyHelper.js]
info: |
    ClassDeclaration : class BindingIdentifier ClassTail

    1. Let className be StringValue of BindingIdentifier.
    2. Let value be the result of ClassDefinitionEvaluation of ClassTail with
       argument className.
    [...]

    14.5.14 Runtime Semantics: ClassDefinitionEvaluation

    21. For each ClassElement m in order from methods
        a. If IsStatic of m is false, then
        b. Else,
           Let status be the result of performing PropertyDefinitionEvaluation for
           m with arguments F and false.
    [...]

    14.4.13 Runtime Semantics: PropertyDefinitionEvaluation

    GeneratorMethod : * PropertyName ( StrictFormalParameters ) { GeneratorBody }

    1. Let propKey be the result of evaluating PropertyName.
    2. ReturnIfAbrupt(propKey).
    3. If the function code for this GeneratorMethod is strict mode code,
       let strict be true. Otherwise let strict be false.
    4. Let scope be the running execution context's LexicalEnvironment.
    5. Let closure be GeneratorFunctionCreate(Method,
       StrictFormalParameters, GeneratorBody, scope, strict).

    9.2.1 [[Call]] ( thisArgument, argumentsList)

    [...]
    7. Let result be OrdinaryCallEvaluateBody(F, argumentsList).
    [...]

    9.2.1.3 OrdinaryCallEvaluateBody ( F, argumentsList )

    1. Let status be FunctionDeclarationInstantiation(F, argumentsList).
    [...]

    9.2.12 FunctionDeclarationInstantiation(func, argumentsList)

    [...]
    23. Let iteratorRecord be Record {[[iterator]]:
        CreateListIterator(argumentsList), [[done]]: false}.
    24. If hasDuplicates is true, then
        [...]
    25. Else,
        b. Let formalStatus be IteratorBindingInitialization for formals with
           iteratorRecord and env as arguments.
    [...]
---*/

var callCount = 0;
class C {
  static *method({a, b, ...rest}) {
    assert.sameValue(rest.a, undefined);
    assert.sameValue(rest.b, undefined);

    verifyProperty(rest, "x", {
      enumerable: true,
      writable: true,
      configurable: true,
      value: 1
    });

    verifyProperty(rest, "y", {
      enumerable: true,
      writable: true,
      configurable: true,
      value: 2
    });
    callCount = callCount + 1;
  }
};

C.method({x: 1, y: 2, a: 5, b: 3}).next();
assert.sameValue(callCount, 1, 'method invoked exactly once');

reportCompare(0, 0);
