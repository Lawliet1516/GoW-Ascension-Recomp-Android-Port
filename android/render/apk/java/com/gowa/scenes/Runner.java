package com.gowa.scenes;
public class Runner { static { System.loadLibrary("gowa_scenes"); }
  public static native int run(String log, String args); public static native void setenv(String k, String v); }
