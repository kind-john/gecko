/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is:
 * http://www.w3.org/html/wg/drafts/html/master/browsers.html#the-window-object
 */

[NamedPropertiesObject]
/*sealed*/ interface Window : EventTarget {
  // the current browsing context
  [Unforgeable] readonly attribute Window window;
  [Replaceable] readonly attribute Window self;
  [Unforgeable] readonly attribute Document document;
           attribute DOMString name; 
  /* [PutForwards=href, Unforgeable] */ readonly attribute Location location;
  /* readonly attribute History history;
  [Replaceable] readonly attribute BarProp locationbar;
  [Replaceable] readonly attribute BarProp menubar;
  [Replaceable] readonly attribute BarProp personalbar;
  [Replaceable] readonly attribute BarProp scrollbars;
  [Replaceable] readonly attribute BarProp statusbar;
  [Replaceable] readonly attribute BarProp toolbar;*/
           attribute DOMString status;
  void close();
  readonly attribute boolean closed;
  void stop();
  void focus();
  void blur();

  // other browsing contexts
  /*[Replaceable] readonly attribute Window frames;
  [Replaceable] readonly attribute unsigned long length;
  [Unforgeable] readonly attribute Window top;
           attribute Window? opener;
           readonly attribute Window parent;*/
  readonly attribute Element? frameElement;
  /*Window open(optional DOMString url = "about:blank", optional DOMString target = "_blank", optional DOMString features = "", optional boolean replace = false);
    getter Window (unsigned long index);*/
  //getter object (DOMString name);

  // the user agent
  readonly attribute Navigator navigator;
  /*
  readonly attribute External external;
  readonly attribute ApplicationCache applicationCache;*/

  // user prompts
  void alert(optional DOMString message = "");
  boolean confirm(optional DOMString message = "");
  DOMString? prompt(optional DOMString message = "", optional DOMString default = "");
  void print();
  any showModalDialog(DOMString url, optional any argument);


};

// Not part of any spec
partial interface Window {
  // web developer niceties
  readonly attribute Console console;
};

/*Window implements GlobalEventHandlers;
  Window implements WindowEventHandlers;*/

[NoInterfaceObject]
interface WindowTimers {
  //long setTimeout(Function handler, optional long timeout = 0, any... arguments);
  //XXXjdm No support for Function or variadic arguments yet
  long setTimeout(any handler, optional long timeout = 0/*, any... arguments*/);
  void clearTimeout(optional long handle = 0);
  long setInterval(any handler, optional long timeout = 0/*, any... arguments*/);
  void clearInterval(optional long handler = 0);
  /*long setTimeout(DOMString handler, optional long timeout = 0, any... arguments);
  long setInterval(Function handler, optional long timeout = 0, any... arguments);
  long setInterval(DOMString handler, optional long timeout = 0, any... arguments);
  void clearInterval(optional long handle = 0);*/
};
Window implements WindowTimers;
