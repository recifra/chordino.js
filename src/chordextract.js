// Appended to the module (--post-js in src/build.sh): the API that
// src/js/chordino.d.mts describes, over the C functions chordextract.c
// exports. Names are quoted so that Closure leaves them as they are.

// What the pushChord and reportProgress EM_JS functions in chordextract.c write to
var chordList = [];
var progressCallback = null;

/** Runs read, which pushes chords one by one, and hands back the list it built */
function readChords(read, instance) {
  chordList = [];
  read(instance);
  return chordList;
}

Module['initializePlugin'] = (sampleRate, blockSize, realtime) =>
  _initializePlugin(sampleRate, blockSize, !!realtime);
Module['processBuffer'] = (instance, buffer, milliseconds) =>
  _processBuffer(instance, buffer, milliseconds);
Module['getResult'] = (instance) => readChords(_getResult, instance);
Module['getPartialResult'] = (instance) => readChords(_getPartialResult, instance);
Module['forgetBefore'] = (instance, milliseconds) => _forgetBefore(instance, milliseconds);
Module['resetProcess'] = (instance) => _resetProcess(instance);
Module['releasePlugin'] = (instance) => _releasePlugin(instance);
Module['setProgressCallback'] = (callback) => {
  progressCallback = callback ?? null;
  _setProgressReports(progressCallback !== null);
};
// EXPORT_KEEPALIVE=0 leaves every C function off Module, these two included
Module['_malloc'] = _malloc;
Module['_free'] = _free;
