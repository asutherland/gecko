Cu.import("resource://services-sync/engines.js");
Cu.import("resource://services-sync/engines/bookmarks.js");
Cu.import("resource://services-sync/util.js");

const PARENT_ANNO = "sync/parent";

Engines.register(BookmarksEngine);
let engine = Engines.get("bookmarks");
let store = engine._store;
let fxuri = Utils.makeURI("http://getfirefox.com/");
let tburi = Utils.makeURI("http://getthunderbird.com/");


function test_bookmark_create() {
  try {
    _("Ensure the record isn't present yet.");
    let ids = Svc.Bookmark.getBookmarkIdsForURI(fxuri, {});
    do_check_eq(ids.length, 0);

    _("Let's create a new record.");
    let fxrecord = new Bookmark("bookmarks", "get-firefox1");
    fxrecord.bmkUri        = fxuri.spec;
    fxrecord.description   = "Firefox is awesome.";
    fxrecord.title         = "Get Firefox!";
    fxrecord.tags          = ["firefox", "awesome", "browser"];
    fxrecord.keyword       = "awesome";
    fxrecord.loadInSidebar = false;
    fxrecord.parentName    = "Bookmarks Toolbar";
    fxrecord.parentid      = "toolbar";
    store.applyIncoming(fxrecord);

    _("Verify it has been created correctly.");
    let id = store.idForGUID(fxrecord.id);
    do_check_eq(store.GUIDForId(id), fxrecord.id);
    do_check_eq(Svc.Bookmark.getItemType(id), Svc.Bookmark.TYPE_BOOKMARK);
    do_check_true(Svc.Bookmark.getBookmarkURI(id).equals(fxuri));
    do_check_eq(Svc.Bookmark.getItemTitle(id), fxrecord.title);
    do_check_eq(Utils.anno(id, "bookmarkProperties/description"),
                fxrecord.description);
    do_check_eq(Svc.Bookmark.getFolderIdForItem(id),
                Svc.Bookmark.toolbarFolder);
    do_check_eq(Svc.Bookmark.getKeywordForBookmark(id), fxrecord.keyword);

    _("Have the store create a new record object. Verify that it has the same data.");
    let newrecord = store.createRecord(fxrecord.id);
    do_check_true(newrecord instanceof Bookmark);
    for each (let property in ["type", "bmkUri", "description", "title",
                               "keyword", "parentName", "parentid"]) {
      do_check_eq(newrecord[property], fxrecord[property]);
    }
    do_check_true(Utils.deepEquals(newrecord.tags.sort(),
                                   fxrecord.tags.sort()));

    _("The calculated sort index is based on frecency data.");
    do_check_true(newrecord.sortindex >= 150);

    _("Create a record with some values missing.");
    let tbrecord = new Bookmark("bookmarks", "thunderbird1");
    tbrecord.bmkUri        = tburi.spec;
    tbrecord.parentName    = "Bookmarks Toolbar";
    tbrecord.parentid      = "toolbar";
    store.applyIncoming(tbrecord);

    _("Verify it has been created correctly.");
    id = store.idForGUID(tbrecord.id);
    do_check_eq(store.GUIDForId(id), tbrecord.id);
    do_check_eq(Svc.Bookmark.getItemType(id), Svc.Bookmark.TYPE_BOOKMARK);
    do_check_true(Svc.Bookmark.getBookmarkURI(id).equals(tburi));
    do_check_eq(Svc.Bookmark.getItemTitle(id), null);
    let error;
    try {
      Utils.anno(id, "bookmarkProperties/description");
    } catch(ex) {
      error = ex;
    }
    do_check_eq(error.result, Cr.NS_ERROR_NOT_AVAILABLE);
    do_check_eq(Svc.Bookmark.getFolderIdForItem(id),
                Svc.Bookmark.toolbarFolder);
    do_check_eq(Svc.Bookmark.getKeywordForBookmark(id), null);
  } finally {
    _("Clean up.");
    store.wipe();
  }
}

function test_bookmark_update() {
  try {
    _("Create a bookmark whose values we'll change.");
    let bmk1_id = Svc.Bookmark.insertBookmark(
      Svc.Bookmark.toolbarFolder, fxuri, Svc.Bookmark.DEFAULT_INDEX,
      "Get Firefox!");
    Utils.anno(bmk1_id, "bookmarkProperties/description", "Firefox is awesome.");
    Svc.Bookmark.setKeywordForBookmark(bmk1_id, "firefox");
    let bmk1_guid = store.GUIDForId(bmk1_id);

    _("Update the record with some null values.");
    let record = store.createRecord(bmk1_guid);
    record.title = null;
    record.description = null;
    record.keyword = null;
    record.tags = null;
    store.applyIncoming(record);

    _("Verify that the values have been cleared.");
    do_check_eq(Utils.anno(bmk1_id, "bookmarkProperties/description"), "");
    do_check_eq(Svc.Bookmark.getItemTitle(bmk1_id), "");
    do_check_eq(Svc.Bookmark.getKeywordForBookmark(bmk1_id), null);
  } finally {
    _("Clean up.");
    store.wipe();
  }
}

function test_bookmark_createRecord() {
  try {
    _("Create a bookmark without a description or title.");
    let bmk1_id = Svc.Bookmark.insertBookmark(
      Svc.Bookmark.toolbarFolder, fxuri, Svc.Bookmark.DEFAULT_INDEX, null);
    let bmk1_guid = store.GUIDForId(bmk1_id);

    _("Verify that the record is created accordingly.");
    let record = store.createRecord(bmk1_guid);
    do_check_eq(record.title, null);
    do_check_eq(record.description, null);
    do_check_eq(record.keyword, null);

  } finally {
    _("Clean up.");
    store.wipe();
  }
}

function test_folder_create() {
  try {
    _("Create a folder.");
    let folder = new BookmarkFolder("bookmarks", "testfolder-1");
    folder.parentName = "Bookmarks Toolbar";
    folder.parentid   = "toolbar";
    folder.title      = "Test Folder";
    store.applyIncoming(folder);

    _("Verify it has been created correctly.");
    let id = store.idForGUID(folder.id);
    do_check_eq(Svc.Bookmark.getItemType(id), Svc.Bookmark.TYPE_FOLDER);
    do_check_eq(Svc.Bookmark.getItemTitle(id), folder.title);
    do_check_eq(Svc.Bookmark.getFolderIdForItem(id),
                Svc.Bookmark.toolbarFolder);

    _("Have the store create a new record object. Verify that it has the same data.");
    let newrecord = store.createRecord(folder.id);
    do_check_true(newrecord instanceof BookmarkFolder);
    for each (let property in ["title", "parentName", "parentid"])
      do_check_eq(newrecord[property], folder[property]);      

    _("Folders have high sort index to ensure they're synced first.");
    do_check_eq(newrecord.sortindex, 1000000);
  } finally {
    _("Clean up.");
    store.wipe();
  }
}

function test_folder_createRecord() {
  try {
    _("Create a folder.");
    let folder1_id = Svc.Bookmark.createFolder(
      Svc.Bookmark.toolbarFolder, "Folder1", 0);
    let folder1_guid = store.GUIDForId(folder1_id);

    _("Create two bookmarks in that folder without assigning them GUIDs.");
    let bmk1_id = Svc.Bookmark.insertBookmark(
      folder1_id, fxuri, Svc.Bookmark.DEFAULT_INDEX, "Get Firefox!");
    let bmk2_id = Svc.Bookmark.insertBookmark(
      folder1_id, tburi, Svc.Bookmark.DEFAULT_INDEX, "Get Thunderbird!");

    _("Create a record for the folder and verify basic properties.");
    let record = store.createRecord(folder1_guid);
    do_check_true(record instanceof BookmarkFolder);
    do_check_eq(record.title, "Folder1");
    do_check_eq(record.parentid, "toolbar");
    do_check_eq(record.parentName, "Bookmarks Toolbar");

    _("Verify the folder's children. Ensures that the bookmarks were given GUIDs.");
    let bmk1_guid = store.GUIDForId(bmk1_id);
    let bmk2_guid = store.GUIDForId(bmk2_id);
    do_check_eq(record.children.length, 2);
    do_check_eq(record.children[0], bmk1_guid);
    do_check_eq(record.children[1], bmk2_guid);

  } finally {
    _("Clean up.");
    store.wipe();
  }
}

function test_deleted() {
  try {
    _("Create a bookmark that will be deleted.");
    let bmk1_id = Svc.Bookmark.insertBookmark(
      Svc.Bookmark.toolbarFolder, fxuri, Svc.Bookmark.DEFAULT_INDEX,
      "Get Firefox!");
    let bmk1_guid = store.GUIDForId(bmk1_id);

    _("Delete the bookmark through the store.");
    let record = new PlacesItem("bookmarks", bmk1_guid);
    record.deleted = true;
    store.applyIncoming(record);

    _("Ensure it has been deleted.");
    let error;
    try {
      Svc.Bookmark.getBookmarkURI(bmk1_id);
    } catch(ex) {
      error = ex;
    }
    do_check_eq(error.result, Cr.NS_ERROR_ILLEGAL_VALUE);

    let newrec = store.createRecord(bmk1_guid);
    do_check_eq(newrec.deleted, true);

  } finally {
    _("Clean up.");
    store.wipe();
  }
}

function test_move_folder() {
  try {
    _("Create two folders and a bookmark in one of them.");
    let folder1_id = Svc.Bookmark.createFolder(
      Svc.Bookmark.toolbarFolder, "Folder1", 0);
    let folder1_guid = store.GUIDForId(folder1_id);
    let folder2_id = Svc.Bookmark.createFolder(
      Svc.Bookmark.toolbarFolder, "Folder2", 0);
    let folder2_guid = store.GUIDForId(folder2_id);
    let bmk_id = Svc.Bookmark.insertBookmark(
      folder1_id, fxuri, Svc.Bookmark.DEFAULT_INDEX, "Get Firefox!");
    let bmk_guid = store.GUIDForId(bmk_id);

    _("Get a record, reparent it and apply it to the store.");
    let record = store.createRecord(bmk_guid);
    do_check_eq(record.parentid, folder1_guid);
    record.parentid = folder2_guid;
    store.applyIncoming(record);

    _("Verify the new parent.");
    let new_folder_id = Svc.Bookmark.getFolderIdForItem(bmk_id);
    do_check_eq(store.GUIDForId(new_folder_id), folder2_guid);
  } finally {
    _("Clean up.");
    store.wipe();
  }
}

function test_move_order() {
  // Make sure the tracker is turned on.
  Svc.Obs.notify("weave:engine:start-tracking");
  try {
    _("Create two bookmarks");
    let bmk1_id = Svc.Bookmark.insertBookmark(
      Svc.Bookmark.toolbarFolder, fxuri, Svc.Bookmark.DEFAULT_INDEX,
      "Get Firefox!");
    let bmk1_guid = store.GUIDForId(bmk1_id);
    let bmk2_id = Svc.Bookmark.insertBookmark(
      Svc.Bookmark.toolbarFolder, tburi, Svc.Bookmark.DEFAULT_INDEX,
      "Get Thunderbird!");
    let bmk2_guid = store.GUIDForId(bmk2_id);

    _("Verify order.");
    do_check_eq(Svc.Bookmark.getItemIndex(bmk1_id), 0);
    do_check_eq(Svc.Bookmark.getItemIndex(bmk2_id), 1);
    let toolbar = store.createRecord("toolbar");
    do_check_eq(toolbar.children.length, 2);
    do_check_eq(toolbar.children[0], bmk1_guid);
    do_check_eq(toolbar.children[1], bmk2_guid);

    _("Move bookmarks around.");
    store._childrenToOrder = {};
    toolbar.children = [bmk2_guid, bmk1_guid];
    store.applyIncoming(toolbar);
    // Bookmarks engine does this at the end of _processIncoming
    engine._tracker.ignoreAll = true;
    store._orderChildren();
    engine._tracker.ignoreAll = false;
    delete store._childrenToOrder;

    _("Verify new order.");
    do_check_eq(Svc.Bookmark.getItemIndex(bmk2_id), 0);
    do_check_eq(Svc.Bookmark.getItemIndex(bmk1_id), 1);

  } finally {
    Svc.Obs.notify("weave:engine:stop-tracking");
    _("Clean up.");
    store.wipe();
  }
}

function test_orphan() {
  try {

    _("Add a new bookmark locally.");
    let bmk1_id = Svc.Bookmark.insertBookmark(
      Svc.Bookmark.toolbarFolder, fxuri, Svc.Bookmark.DEFAULT_INDEX,
      "Get Firefox!");
    let bmk1_guid = store.GUIDForId(bmk1_id);
    do_check_eq(Svc.Bookmark.getFolderIdForItem(bmk1_id), Svc.Bookmark.toolbarFolder);
    let error;
    try {
      Utils.anno(bmk1_id, PARENT_ANNO);
    } catch(ex) {
      error = ex;
    }
    do_check_eq(error.result, Cr.NS_ERROR_NOT_AVAILABLE);

    _("Apply a server record that is the same but refers to non-existent folder.");
    let record = store.createRecord(bmk1_guid);
    record.parentid = "non-existent";
    store.applyIncoming(record);

    _("Verify that bookmark has been flagged as orphan, has not moved.");
    do_check_eq(Svc.Bookmark.getFolderIdForItem(bmk1_id), Svc.Bookmark.toolbarFolder);
    do_check_eq(Utils.anno(bmk1_id, PARENT_ANNO), "non-existent");

  } finally {
    _("Clean up.");
    store.wipe();
  }
}

function test_reparentOrphans() {
  try {
    let folder1_id = Svc.Bookmark.createFolder(
      Svc.Bookmark.toolbarFolder, "Folder1", 0);
    let folder1_guid = store.GUIDForId(folder1_id);

    _("Create a bogus orphan record and write the record back to the store to trigger _reparentOrphans.");
    Utils.anno(folder1_id, PARENT_ANNO, folder1_guid);
    let record = store.createRecord(folder1_guid);
    record.title = "New title for Folder 1";
    store._childrenToOrder = {};
    store.applyIncoming(record);

    _("Verify that is has been marked as an orphan even though it couldn't be moved into itself.");
    do_check_eq(Utils.anno(folder1_id, PARENT_ANNO), folder1_guid);

  } finally {
    _("Clean up.");
    store.wipe();
  }
}

// Copying a bookmark in the UI also copies its annotations, including the GUID
// annotation in 3.x.
function test_copying_avoid_duplicate_guids() {
  if (store._haveGUIDColumn) {
    _("No GUID annotation handling functionality; returning without testing.");
    return;
  }
    
  try {
    _("Ensure the record isn't present yet.");
    let ids = Svc.Bookmark.getBookmarkIdsForURI(fxuri, {});
    do_check_eq(ids.length, 0);

    _("Let's create a new record.");
    let fxrecord = new Bookmark("bookmarks", "get-firefox1");
    fxrecord.bmkUri        = fxuri.spec;
    fxrecord.description   = "Firefox is awesome.";
    fxrecord.title         = "Get Firefox!";
    fxrecord.tags          = ["firefox", "awesome", "browser"];
    fxrecord.keyword       = "awesome";
    fxrecord.loadInSidebar = false;
    fxrecord.parentName    = "Bookmarks Toolbar";
    fxrecord.parentid      = "toolbar";
    store.applyIncoming(fxrecord);

    _("Verify it has been created correctly.");
    let id = store.idForGUID(fxrecord.id);
    do_check_eq(store.GUIDForId(id), fxrecord.id);
    do_check_true(Svc.Bookmark.getBookmarkURI(id).equals(fxuri));
    do_check_eq(Utils.anno(id, "bookmarkProperties/description"),
                fxrecord.description);
    
    _("Copy the record as happens in the UI: with the same GUID.");
    let parentID = Svc.Bookmark.getFolderIdForItem(id);
    let copy = Svc.Bookmark.insertBookmark(parentID, fxuri, -1, fxrecord.title);
    Svc.Bookmark.setItemLastModified(copy, Svc.Bookmark.getItemLastModified(id));
    Svc.Bookmark.setItemDateAdded(copy, Svc.Bookmark.getItemDateAdded(id));
    store._setGUID(copy, fxrecord.id);
    
    do_check_eq(store.GUIDForId(copy), store.GUIDForId(id));
    
    _("Calling idForGUID fixes things.");
    _("GUID before: " + store.GUIDForId(copy));
    do_check_eq(store.idForGUID(fxrecord.id), id);           // Oldest wins.
    _("GUID now: " + store.GUIDForId(copy));
    do_check_neq(store.GUIDForId(copy), store.GUIDForId(id));
    
    _("Verify that the anno itself has changed.");
    do_check_neq(Utils.anno(copy, "sync/guid"), fxrecord.id);
    do_check_eq(Utils.anno(copy, "sync/guid"), store.GUIDForId(copy));
    
  } finally {
    _("Clean up.");
    store.wipe();
  }
}

function run_test() {
  initTestLogging('Trace');
  test_bookmark_create();
  test_bookmark_createRecord();
  test_bookmark_update();
  test_folder_create();
  test_folder_createRecord();
  test_deleted();
  test_move_folder();
  test_move_order();
  test_orphan();
  test_reparentOrphans();
  test_copying_avoid_duplicate_guids();
}
