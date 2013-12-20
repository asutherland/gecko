/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

/**
 * Make sure that the variables view remains responsive when faced with
 * huge ammounts of data.
 */

const TAB_URL = EXAMPLE_URL + "doc_large-array-buffer.html";

let gTab, gDebuggee, gPanel, gDebugger;
let gVariables, gEllipsis;

function test() {
  initDebugger(TAB_URL).then(([aTab, aDebuggee, aPanel]) => {
    gTab = aTab;
    gDebuggee = aDebuggee;
    gPanel = aPanel;
    gDebugger = gPanel.panelWin;
    gVariables = gDebugger.DebuggerView.Variables;
    gEllipsis = Services.prefs.getComplexValue("intl.ellipsis", Ci.nsIPrefLocalizedString).data;

    waitForSourceAndCaretAndScopes(gPanel, ".html", 23)
      .then(() => initialChecks())
      .then(() => verifyFirstLevel())
      .then(() => verifyNextLevels())
      .then(() => resumeDebuggerThenCloseAndFinish(gPanel))
      .then(null, aError => {
        ok(false, "Got an error: " + aError.message + "\n" + aError.stack);
      });

    EventUtils.sendMouseEvent({ type: "click" },
      gDebuggee.document.querySelector("button"),
      gDebuggee);
  });
}

function initialChecks() {
  let localScope = gVariables.getScopeAtIndex(0);
  let bufferVar = localScope.get("buffer");
  let arrayVar = localScope.get("largeArray");
  let objectVar = localScope.get("largeObject");

  ok(bufferVar, "There should be a 'buffer' variable present in the scope.");
  ok(arrayVar, "There should be a 'largeArray' variable present in the scope.");
  ok(objectVar, "There should be a 'largeObject' variable present in the scope.");

  is(bufferVar.target.querySelector(".name").getAttribute("value"), "buffer",
    "Should have the right property name for 'buffer'.");
  is(bufferVar.target.querySelector(".value").getAttribute("value"), "ArrayBuffer",
    "Should have the right property value for 'buffer'.");
  ok(bufferVar.target.querySelector(".value").className.contains("token-other"),
    "Should have the right token class for 'buffer'.");

  is(arrayVar.target.querySelector(".name").getAttribute("value"), "largeArray",
    "Should have the right property name for 'largeArray'.");
  is(arrayVar.target.querySelector(".value").getAttribute("value"), "Int8Array",
    "Should have the right property value for 'largeArray'.");
  ok(arrayVar.target.querySelector(".value").className.contains("token-other"),
    "Should have the right token class for 'largeArray'.");

  is(objectVar.target.querySelector(".name").getAttribute("value"), "largeObject",
    "Should have the right property name for 'largeObject'.");
  is(objectVar.target.querySelector(".value").getAttribute("value"), "Object",
    "Should have the right property value for 'largeObject'.");
  ok(objectVar.target.querySelector(".value").className.contains("token-other"),
    "Should have the right token class for 'largeObject'.");

  is(bufferVar.expanded, false,
    "The 'buffer' variable shouldn't be expanded.");
  is(arrayVar.expanded, false,
    "The 'largeArray' variable shouldn't be expanded.");
  is(objectVar.expanded, false,
    "The 'largeObject' variable shouldn't be expanded.");

  let finished = waitForDebuggerEvents(gPanel, gDebugger.EVENTS.FETCHED_PROPERTIES, 2);
  arrayVar.expand();
  objectVar.expand();
  return finished;
}

function verifyFirstLevel() {
  let localScope = gVariables.getScopeAtIndex(0);
  let arrayVar = localScope.get("largeArray");
  let objectVar = localScope.get("largeObject");

  let arrayEnums = arrayVar.target.querySelector(".variables-view-element-details.enum").childNodes;
  let arrayNonEnums = arrayVar.target.querySelector(".variables-view-element-details.nonenum").childNodes;
  is(arrayEnums.length, 0,
    "The 'largeArray' shouldn't contain any enumerable elements.");
  is(arrayNonEnums.length, 9,
    "The 'largeArray' should contain all the created non-enumerable elements.");

  let objectEnums = objectVar.target.querySelector(".variables-view-element-details.enum").childNodes;
  let objectNonEnums = objectVar.target.querySelector(".variables-view-element-details.nonenum").childNodes;
  is(objectEnums.length, 0,
    "The 'largeObject' shouldn't contain any enumerable elements.");
  is(objectNonEnums.length, 5,
    "The 'largeObject' should contain all the created non-enumerable elements.");

  is(arrayVar.target.querySelectorAll(".variables-view-property .name")[0].getAttribute("value"),
    0 + gEllipsis + 1999, "The first page in the 'largeArray' is named correctly.");
  is(arrayVar.target.querySelectorAll(".variables-view-property .value")[0].getAttribute("value"),
    "", "The first page in the 'largeArray' should not have a corresponding value.");
  is(arrayVar.target.querySelectorAll(".variables-view-property .name")[1].getAttribute("value"),
    2000 + gEllipsis + 3999, "The second page in the 'largeArray' is named correctly.");
  is(arrayVar.target.querySelectorAll(".variables-view-property .value")[1].getAttribute("value"),
    "", "The second page in the 'largeArray' should not have a corresponding value.");
  is(arrayVar.target.querySelectorAll(".variables-view-property .name")[2].getAttribute("value"),
    4000 + gEllipsis + 5999, "The third page in the 'largeArray' is named correctly.");
  is(arrayVar.target.querySelectorAll(".variables-view-property .value")[2].getAttribute("value"),
    "", "The third page in the 'largeArray' should not have a corresponding value.");
  is(arrayVar.target.querySelectorAll(".variables-view-property .name")[3].getAttribute("value"),
    6000 + gEllipsis + 9999, "The fourth page in the 'largeArray' is named correctly.");
  is(arrayVar.target.querySelectorAll(".variables-view-property .value")[3].getAttribute("value"),
    "", "The fourth page in the 'largeArray' should not have a corresponding value.");

  is(objectVar.target.querySelectorAll(".variables-view-property .name")[0].getAttribute("value"),
    0 + gEllipsis + 1999, "The first page in the 'largeObject' is named correctly.");
  is(objectVar.target.querySelectorAll(".variables-view-property .value")[0].getAttribute("value"),
    "", "The first page in the 'largeObject' should not have a corresponding value.");
  is(objectVar.target.querySelectorAll(".variables-view-property .name")[1].getAttribute("value"),
    2000 + gEllipsis + 3999, "The second page in the 'largeObject' is named correctly.");
  is(objectVar.target.querySelectorAll(".variables-view-property .value")[1].getAttribute("value"),
    "", "The second page in the 'largeObject' should not have a corresponding value.");
  is(objectVar.target.querySelectorAll(".variables-view-property .name")[2].getAttribute("value"),
    4000 + gEllipsis + 5999, "The thrid page in the 'largeObject' is named correctly.");
  is(objectVar.target.querySelectorAll(".variables-view-property .value")[2].getAttribute("value"),
    "", "The thrid page in the 'largeObject' should not have a corresponding value.");
  is(objectVar.target.querySelectorAll(".variables-view-property .name")[3].getAttribute("value"),
    6000 + gEllipsis + 9999, "The fourth page in the 'largeObject' is named correctly.");
  is(objectVar.target.querySelectorAll(".variables-view-property .value")[3].getAttribute("value"),
    "", "The fourth page in the 'largeObject' should not have a corresponding value.");

  is(arrayVar.target.querySelectorAll(".variables-view-property .name")[4].getAttribute("value"),
    "length", "The other properties 'largeArray' are named correctly.");
  is(arrayVar.target.querySelectorAll(".variables-view-property .value")[4].getAttribute("value"),
    "10000", "The other properties 'largeArray' have the correct value.");
  is(arrayVar.target.querySelectorAll(".variables-view-property .name")[5].getAttribute("value"),
    "buffer", "The other properties 'largeArray' are named correctly.");
  is(arrayVar.target.querySelectorAll(".variables-view-property .value")[5].getAttribute("value"),
    "ArrayBuffer", "The other properties 'largeArray' have the correct value.");
  is(arrayVar.target.querySelectorAll(".variables-view-property .name")[6].getAttribute("value"),
    "byteLength", "The other properties 'largeArray' are named correctly.");
  is(arrayVar.target.querySelectorAll(".variables-view-property .value")[6].getAttribute("value"),
    "10000", "The other properties 'largeArray' have the correct value.");
  is(arrayVar.target.querySelectorAll(".variables-view-property .name")[7].getAttribute("value"),
    "byteOffset", "The other properties 'largeArray' are named correctly.");
  is(arrayVar.target.querySelectorAll(".variables-view-property .value")[7].getAttribute("value"),
    "0", "The other properties 'largeArray' have the correct value.");
  is(arrayVar.target.querySelectorAll(".variables-view-property .name")[8].getAttribute("value"),
    "__proto__", "The other properties 'largeArray' are named correctly.");
  is(arrayVar.target.querySelectorAll(".variables-view-property .value")[8].getAttribute("value"),
    "Int8ArrayPrototype", "The other properties 'largeArray' have the correct value.");

  is(objectVar.target.querySelectorAll(".variables-view-property .name")[4].getAttribute("value"),
    "__proto__", "The other properties 'largeObject' are named correctly.");
  is(objectVar.target.querySelectorAll(".variables-view-property .value")[4].getAttribute("value"),
    "Object", "The other properties 'largeObject' have the correct value.");
}

function verifyNextLevels() {
  let localScope = gVariables.getScopeAtIndex(0);
  let objectVar = localScope.get("largeObject");

  let lastPage1 = objectVar.get(6000 + gEllipsis + 9999);
  ok(lastPage1, "The last page in the first level was retrieved successfully.");
  lastPage1.expand();

  let pageEnums1 = lastPage1.target.querySelector(".variables-view-element-details.enum").childNodes;
  let pageNonEnums1 = lastPage1.target.querySelector(".variables-view-element-details.nonenum").childNodes;
  is(pageEnums1.length, 0,
    "The last page in the first level shouldn't contain any enumerable elements.");
  is(pageNonEnums1.length, 4,
    "The last page in the first level should contain all the created non-enumerable elements.");

  is(lastPage1._nonenum.querySelectorAll(".variables-view-property .name")[0].getAttribute("value"),
    6000 + gEllipsis + 6999, "The first page in this level named correctly (1).");
  is(lastPage1._nonenum.querySelectorAll(".variables-view-property .name")[1].getAttribute("value"),
    7000 + gEllipsis + 7999, "The second page in this level named correctly (1).");
  is(lastPage1._nonenum.querySelectorAll(".variables-view-property .name")[2].getAttribute("value"),
    8000 + gEllipsis + 8999, "The third page in this level named correctly (1).");
  is(lastPage1._nonenum.querySelectorAll(".variables-view-property .name")[3].getAttribute("value"),
    9000 + gEllipsis + 9999, "The fourth page in this level named correctly (1).");

  let lastPage2 = lastPage1.get(9000 + gEllipsis + 9999);
  ok(lastPage2, "The last page in the second level was retrieved successfully.");
  lastPage2.expand();

  let pageEnums2 = lastPage2.target.querySelector(".variables-view-element-details.enum").childNodes;
  let pageNonEnums2 = lastPage2.target.querySelector(".variables-view-element-details.nonenum").childNodes;
  is(pageEnums2.length, 0,
    "The last page in the second level shouldn't contain any enumerable elements.");
  is(pageNonEnums2.length, 4,
    "The last page in the second level should contain all the created non-enumerable elements.");

  is(lastPage2._nonenum.querySelectorAll(".variables-view-property .name")[0].getAttribute("value"),
    9000 + gEllipsis + 9199, "The first page in this level named correctly (2).");
  is(lastPage2._nonenum.querySelectorAll(".variables-view-property .name")[1].getAttribute("value"),
    9200 + gEllipsis + 9399, "The second page in this level named correctly (2).");
  is(lastPage2._nonenum.querySelectorAll(".variables-view-property .name")[2].getAttribute("value"),
    9400 + gEllipsis + 9599, "The third page in this level named correctly (2).");
  is(lastPage2._nonenum.querySelectorAll(".variables-view-property .name")[3].getAttribute("value"),
    9600 + gEllipsis + 9999, "The fourth page in this level named correctly (2).");

  let lastPage3 = lastPage2.get(9600 + gEllipsis + 9999);
  ok(lastPage3, "The last page in the third level was retrieved successfully.");
  lastPage3.expand();

  let pageEnums3 = lastPage3.target.querySelector(".variables-view-element-details.enum").childNodes;
  let pageNonEnums3 = lastPage3.target.querySelector(".variables-view-element-details.nonenum").childNodes;
  is(pageEnums3.length, 400,
    "The last page in the third level should contain all the created enumerable elements.");
  is(pageNonEnums3.length, 0,
    "The last page in the third level shouldn't contain any non-enumerable elements.");

  is(lastPage3._enum.querySelectorAll(".variables-view-property .name")[0].getAttribute("value"),
    9600, "The properties in this level are named correctly (3).");
  is(lastPage3._enum.querySelectorAll(".variables-view-property .name")[1].getAttribute("value"),
    9601, "The properties in this level are named correctly (3).");
  is(lastPage3._enum.querySelectorAll(".variables-view-property .name")[398].getAttribute("value"),
    9998, "The properties in this level are named correctly (3).");
  is(lastPage3._enum.querySelectorAll(".variables-view-property .name")[399].getAttribute("value"),
    9999, "The properties in this level are named correctly (3).");

  is(lastPage3._enum.querySelectorAll(".variables-view-property .value")[0].getAttribute("value"),
    399, "The properties in this level have the correct value (3).");
  is(lastPage3._enum.querySelectorAll(".variables-view-property .value")[1].getAttribute("value"),
    398, "The properties in this level have the correct value (3).");
  is(lastPage3._enum.querySelectorAll(".variables-view-property .value")[398].getAttribute("value"),
    1, "The properties in this level have the correct value (3).");
  is(lastPage3._enum.querySelectorAll(".variables-view-property .value")[399].getAttribute("value"),
    0, "The properties in this level have the correct value (3).");
}

registerCleanupFunction(function() {
  gTab = null;
  gDebuggee = null;
  gPanel = null;
  gDebugger = null;
  gVariables = null;
  gEllipsis = null;
});
