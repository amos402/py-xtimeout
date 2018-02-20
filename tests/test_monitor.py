import functools
import random
import threading
import time
import unittest

import pymonitor


def busy(seconds):
	end = time.process_time() + seconds
	while time.process_time() < end:
		for i in range(random.randint(1000, 3000)):
			pass


class TimeoutError(Exception):
	pass


class TestMonitor(unittest.TestCase):
	def test_with(self):
		def on_timout(start_time):
			nonlocal called
			called = True

		called = False
		with pymonitor.check_context(10, on_timout):
			busy(0.1)
		self.assertTrue(called)

	def test_with_nest(self):
		def on_timeout_1(start_time):
			nonlocal called1
			self.assertGreaterEqual(time.process_time() - start_time, 0.05)
			called1 = True

		def on_timeout_2(start_time):
			nonlocal called2
			nonlocal count
			count += 1
			called2 = True

		called1 = False
		called2 = False
		count = 0

		start = time.process_time()
		with pymonitor.check_context(50, on_timeout_1):
			start1 = time.clock()
			while time.clock() - start1 < 0.1:
				for i in range(2):
					start2 = time.clock()
					with pymonitor.check_context(10, on_timeout_2):
						busy(0.1)

		self.assertEqual(count, 2)
		self.assertTrue(called1)
		self.assertTrue(called2)

	def test_break(self):
		def on_timeout(start_time):
			raise TimeoutError

		with self.assertRaises(TimeoutError):
			with pymonitor.check_context(50, on_timeout):
				busy(0.1)

	def test_decorator(self):
		def on_timeout(start_time):
			raise Exception("Timeout")

		@pymonitor.check_time(10, on_timeout)
		def func():
			busy(1)

		with self.assertRaises(Exception) as context:
			func()
		self.assertEqual(context.exception.args[0], "Timeout")

	def test_child_thread(self):
		def on_timeout(start_time):
			raise TimeoutError

		def thfunc():
			with self.assertRaises(TimeoutError):
				with pymonitor.check_context(50, on_timeout):
					busy(0.1)

		th = threading.Thread(target=thfunc)
		th.start()
		th.join()

	def test_multi_threads(self):
		def on_timeout(start_time):
			nonlocal count
			count += 1
		count = 0
		def thfunc():
			with pymonitor.check_context(50, on_timeout):
				busy(0.1)
		ths = []
		for i in range(4):
			th = threading.Thread(target=thfunc)
			th.start()
			ths.append(th)
		for th in ths:
			th.join()
		self.assertEqual(count, 4)


if __name__ == "__main__":
	unittest.main()
