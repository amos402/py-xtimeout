`中文 <https://github.com/amos402/py-xtimeout/blob/master/README_zh.md>`__
=======================

Check function with timeout for tracing or handle it
====================================================

Feature
=======

-  Check a function call is timeout or not.
-  The timeout callback and function call are on the same thread.
-  Multi-thread support. Nest call support.

Usage
=====

.. code:: python

    def on_timeout(start_time: float):
        """
        :param start_time: 
        """
        traceback.print_stack()
        pdb.set_trace()
        raise Exception("time_out")

    # time unit is millisecond
    @pymonitor.check_time(10, on_timeout)
    def function_1():
        pass

    def function_2():
        with pymonitor.check_context(20, on_timeout):
            # do something
            with pymonitor.check_context(10, on_timeout):
                # do something

Implementation Comparison
=========================

Here are some comparisons of the other implementations.

-  Use ``signal`` module and emit a signal

   -  Only works in main thread.
   -  Not good for nest call becauseof one signal correspond one
      handler.
      If you need nest support you need to enter the timeout function
      continually
      and call ``alarm``. The cost depend on your accuracy.
   -  Support Linux only.

-  Start new thread for work and join it with a time, if it had timeout,
   handle with it (eg. terminate it)

   -  Can’t inject the function call.
   -  Overhead from threading.

-  Use ``sys.settrace`` keep tracing for each

   -  A huge cost for that.
