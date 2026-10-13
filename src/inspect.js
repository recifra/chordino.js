// Appended to the inspect build (--post-js in src/build.sh), after
// chordextract.js: getAnalysis and getPartialAnalysis over the C functions
// inspect.c exports.

// What the EM_JS functions in inspect.c build
var analysis = null;

Module['getAnalysis'] = (instance) => {
  _getAnalysis(instance);
  return analysis;
};
Module['getPartialAnalysis'] = (instance, afterSeconds) => {
  _getPartialAnalysis(instance, afterSeconds);
  return analysis;
};
