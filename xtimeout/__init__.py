import functools

from _xtimeout import Injector

__version__ = "0.2.0"
__all__ = ["check_context", "check_time", "Injector"]


class check_context(object):
    def __init__(self, timeout, callback):
        self._injector = Injector(timeout, callback)

    def __enter__(self):
        self._injector.start()
        return self

    def __exit__(self, exc_type, exc_value, tb):
        self._injector.stop()


def check_time(timeout, callback):
    def decorate(func):
        @functools.wraps(func)
        def wrapper(*args, **kwargs):
            with check_context(timeout, callback):
                return func(*args, **kwargs)
        return wrapper
    return decorate
