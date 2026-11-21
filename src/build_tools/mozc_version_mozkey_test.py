"""Independent regression tests for Mozkey release-to-MSI version mapping."""

import os
import tempfile
import unittest

from build_tools import mozc_version


class MozkeyReleaseVersionTest(unittest.TestCase):

  def _read_version(self, major, minor, patch):
    values = {
        'MAJOR': '3',
        'MINOR': '33',
        'BUILD': '6153',
        'REVISION': '100',
        'MOZKEY_RELEASE_VERSION_MAJOR': str(major),
        'MOZKEY_RELEASE_VERSION_MINOR': str(minor),
        'MOZKEY_RELEASE_VERSION_PATCH': str(patch),
        'TARGET_PLATFORM': 'Windows',
        'BUILD_OSS': '6153',
        'QT_VERSION': '6.9.1',
        'ENGINE_VERSION': '24',
        'DATA_VERSION': '11',
    }
    with tempfile.TemporaryDirectory() as directory:
      version_file = os.path.join(directory, 'mozc_version.txt')
      with open(version_file, 'w', encoding='utf-8') as file:
        for key, value in values.items():
          file.write('%s=%s\n' % (key, value))
      result = mozc_version.MozcVersion(version_file)
    return result

  def test_mapping_and_independence(self):
    for major, minor, patch, expected in [
        (0, 7, 7, '100.7.7'),
        (0, 7, 8, '100.7.8'),
        (0, 8, 0, '100.8.0'),
        (1, 0, 0, '101.0.0'),
        (155, 255, 65535, '255.255.65535'),
    ]:
      with self.subTest(version=(major, minor, patch)):
        version = self._read_version(major, minor, patch)
        self.assertEqual(
            version.GetMozkeyReleaseVersion(), '%d.%d.%d' % (major, minor, patch))
        self.assertEqual(version.GetMozkeyMsiVersion(), expected)
        self.assertEqual(version.GetVersionString(), '3.33.6153.100')

  def test_out_of_range_and_invalid(self):
    for major, minor, patch in [
        (156, 0, 0),
        (0, 256, 0),
        (0, 0, 65536),
        (-1, 0, 0),
        ('01', 0, 0),
        ('x', 0, 0),
    ]:
      with self.subTest(version=(major, minor, patch)):
        version = self._read_version(major, minor, patch)
        with self.assertRaises(ValueError):
          version.GetMozkeyMsiVersion()


if __name__ == '__main__':
  unittest.main()
