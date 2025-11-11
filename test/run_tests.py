import unittest, sys

if __name__ == "__main__":
    suite = unittest.defaultTestLoader.discover("test")
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
