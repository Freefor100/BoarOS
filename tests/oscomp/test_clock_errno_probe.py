"""Attribution needs the actual clock result, errno transitions and native exit."""
import unittest
from clock_errno_probe import observation

class ErrnoObservationTests(unittest.TestCase):
    def stream(self,random_result,errno,clock_result=0):
        return ('MALLOC-INIT pc=1 caller=2 errno=0\n'
                f'RETURN nr=278 pc=3 result={random_result} errno=0\n'
                + ('ERRNO-STORE pc=7e85a offset=136 value=11 asm=sw\n' if errno else '') +
                f'MAIN pc=4 errno={errno}\n'
                f'CLOCK-RESULT pc=5 result={clock_result} errno={errno}\n')

    def test_distinguishes_successful_clock_with_prior_errno_from_clock_error(self):
        result=observation(self.stream(-11,11),'AUDIT WAIT pid=2 status=256')
        self.assertEqual((result['getrandom_return'],result['clock_return'],result['clock_errno']),(-11,0,11))
        with self.assertRaises(ValueError):
            observation(self.stream(-11,11,-1),'AUDIT WAIT pid=2 status=256')

    def test_missing_main_duplicate_wait_or_unexplained_failure_cannot_prove_cause(self):
        valid=self.stream(8,0)
        self.assertEqual(observation(valid,'AUDIT WAIT pid=2 status=0')['main_errno'],0)
        for text,serial in [(valid.replace('MAIN pc=4 errno=0\n',''),'AUDIT WAIT pid=2 status=0'),
                            (valid,'AUDIT WAIT pid=2 status=0\nAUDIT WAIT pid=3 status=0'),
                            (valid,'AUDIT WAIT pid=2 status=256')]:
            with self.subTest(text=text,serial=serial),self.assertRaises(ValueError):observation(text,serial)

    def test_failure_without_the_errno_writer_does_not_prove_attribution(self):
        text=self.stream(-11,11).replace('ERRNO-STORE pc=7e85a offset=136 value=11 asm=sw\n','')
        with self.assertRaises(ValueError):observation(text,'AUDIT WAIT pid=2 status=256')

if __name__=='__main__':unittest.main()
