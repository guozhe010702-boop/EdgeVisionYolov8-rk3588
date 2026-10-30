"""Offline regression tests; no camera, SSH or RKNN is accessed."""
import tempfile
import unittest
from pathlib import Path
from board_control import stream_diagnostics


class StatusTests(unittest.TestCase):
    def inspect(self, content, running=False):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'stream.log'
            path.write_text(content)
            return stream_diagnostics(str(path), running)

    def test_rknn_failure_marks_stopped_session_failed(self):
        result = self.inspect('METRIC {"frames":76380}\nE RKNN: failed to submit!\nrknn_run fail! ret=-1\n')
        self.assertEqual(result['phase'], 'failed')
        self.assertEqual(result['error']['code'], 'rknn_inference_failed')
        self.assertTrue(result['metrics_stale'])

    def test_running_session_does_not_inherit_failure_state(self):
        result = self.inspect('rknn_run fail\nMETRIC {"frames":5}\n', True)
        self.assertEqual(result['phase'], 'running')
        self.assertIsNone(result['error'])
        self.assertIsNone(result['metrics_stale'])

    def test_clean_stop_is_not_failure(self):
        self.assertEqual(self.inspect('stream done, total frames=10')['phase'], 'stopped')

    def test_malformed_metric_does_not_break_status(self):
        result = self.inspect('METRIC {"frames":1}\nMETRIC {truncated\n', True)
        self.assertEqual(result['last_metric']['frames'], 1)

    def test_missing_log_is_unavailable(self):
        result = stream_diagnostics(None, True)
        self.assertIsNone(result['last_metric'])
        self.assertTrue(result['metrics_stale'])


if __name__ == '__main__':
    unittest.main()
