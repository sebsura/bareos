.. _GrpcPlugin:

Grpc Plugin
~~~~~~~~~~~

.. index::
   pair: Plugin; Grpc

The grpc plugin is a plugin that allows you to run a separate executable as a bareos plugin.
This executable talks to the core via [grpc](https://grpc.io) remote procedure calls.

This has multiple upsides for users:

   * A crash inside a plugin will not also crash the daemon,
   * eliminates some classes of concurrency problems related to the use of global state inside plugins,
   * ...

This plugin on its own is not very useful.  Its only a bridge between the bareos core and
the actual plugin doing the work.

The plugin comes with two executables, `grpc-test-module` and `grpc-python-module`, which allow you
to make use of this bridge.
As the name suggests, `grpc-test-module` is a simple module that can be used to test that the bridge is working.
`grpc-python-module` on the other hand is a handy little executable that can be used to load and run
normal bareos plugins (including python plugins) in a separate process.

Status of the Grpc Plugin
^^^^^^^^^^^^^^^^^^^^^^^^^
This plugin is still in an **experimental** phase.  The API between core and plugins may change at any time.


Installation of the Grpc Plugin
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
The grpc plugin, together with the `grpc-test-module` and `grpc-python-module`, can be installed with the
**bareos-filedaemon-gprc-plugin** package.

Configuration of the Grpc Plugin
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
The Grpc Plugin receives as first argument the name of the executable that it should execute.
All other options are then passed to that executable.  E.g. if you want to start the python3 plugin `pyplug`
in a separate procces, your configuration would look like this:

.. code-block:: bareosconfig
   :caption: /etc/bareos/bareos-dir.d/fileset/GrpcPython.conf

   FileSet {
     Name = "GrpcPython"
     Description = "Run a python plugin in a separate process"
     Include {
       Options {
         Signature = XXH128
       }
       Plugin = "grpc-fd"
                ":grpc-python-module"
                ":python3"
                ":module_name=pyplug"
                ":arg1=val1"
                ...
     }
   }

Currently the plugin itself does not take any arguments except for the executable name.
The executable name is interpreted as a relative path starting in the bareos plugin directory.
