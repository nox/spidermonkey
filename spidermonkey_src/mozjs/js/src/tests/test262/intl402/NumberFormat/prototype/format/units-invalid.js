// |reftest| skip-if(release_or_beta) -- Intl.NumberFormat-unified is not released yet
// Copyright 2019 Igalia, S.L., Google, Inc. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-intl.numberformat.prototype.format
description: Checks handling of units.
features: [Intl.NumberFormat-unified]
---*/

const units = [
  "acre-foot",
  "ampere",
  "arc-minute",
  "arc-second",
  "astronomical-unit",
  "atmosphere",
  "bushel",
  "calorie",
  "carat",
  "centiliter",
  "century",
  "cubic-centimeter",
  "cubic-foot",
  "cubic-inch",
  "cubic-kilometer",
  "cubic-meter",
  "cubic-mile",
  "cubic-yard",
  "cup-metric",
  "cup",
  "day-person",
  "deciliter",
  "decimeter",
  "fathom",
  "foodcalorie",
  "furlong",
  "g-force",
  "gallon-imperial",
  "generic",
  "gigahertz",
  "gigawatt",
  "hectoliter",
  "hectopascal",
  "hertz",
  "horsepower",
  "inch-hg",
  "joule",
  "karat",
  "kelvin",
  "kilocalorie",
  "kilohertz",
  "kilojoule",
  "kilowatt-hour",
  "kilowatt",
  "knot",
  "light-year",
  "lux",
  "megahertz",
  "megaliter",
  "megawatt",
  "metric-ton",
  "microgram",
  "micrometer",
  "microsecond",
  "milliampere",
  "millibar",
  "milligram",
  "millimeter-of-mercury",
  "milliwatt",
  "month-person",
  "nanometer",
  "nanosecond",
  "nautical-mile",
  "ohm",
  "ounce-troy",
  "parsec",
  "permille",
  "picometer",
  "pint-metric",
  "pint",
  "point",
  "quart",
  "radian",
  "revolution",
  "square-centimeter",
  "square-foot",
  "square-inch",
  "square-kilometer",
  "square-meter",
  "square-mile",
  "square-yard",
  "tablespoon",
  "teaspoon",
  "ton",
  "volt",
  "watt",
  "week-person",
  "year-person",
  "liter-per-100kilometers",
  "meter-per-second-squared",
  "mile-per-gallon-imperial",
  "milligram-per-deciliter",
  "millimole-per-liter",
  "part-per-million",
  "pound-per-square-inch",
];

for (const unit of units) {
  assert.throws(RangeError, () => new Intl.NumberFormat(undefined, { style: "unit", unit }), `Throw for ${unit}`);
}

reportCompare(0, 0);
