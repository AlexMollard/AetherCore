import copy
import unittest
import distribution

class DistributionPolicyTests(unittest.TestCase):
 def test_required_checks_cannot_be_skipped_or_missing(self):
  good={name: {'status':'passed'} for name in distribution.REQUIRED}
  self.assertEqual(distribution.qualify(good),'passed')
  for name in distribution.REQUIRED:
   broken=copy.deepcopy(good);broken[name]['status']='skipped'
   self.assertEqual(distribution.qualify(broken),'blocked')
   del broken[name];self.assertEqual(distribution.qualify(broken),'blocked')
 def test_native_gauntlet_requires_full_phases(self):
  report={'overall':'PASS','mode':'full','phases':[{'name':x,'status':'pass'} for x in ('guard','build','unit','editor','runtime')]}
  self.assertTrue(distribution.native_passed(report))
  report['phases'][-1]['status']='skip';self.assertFalse(distribution.native_passed(report))
 def test_browser_gauntlet_requires_all_reference_cases(self):
  report={'status':'passed','cases':[{'name':str(i),'status':'passed'} for i in range(10)],'legacy':{'status':'passed','cases':[{'name':str(i),'status':'passed'} for i in range(31)]},'cleanup':{'browserClosed':True,'serverClosed':True,'childClosed':True}}
  self.assertTrue(distribution.browser_passed(report))
  report['legacy']['cases'].pop();self.assertFalse(distribution.browser_passed(report))
 def test_executed_but_unfinished_checks_are_not_called_unexecuted(self):
  for cancelled in (False,True):
   checks={'compile':{'status':'running'},'already-done':{'status':'passed'}}
   distribution.finish_unvalidated(checks,cancelled)
   self.assertEqual(checks['compile']['status'],'cancelled' if cancelled else 'failed')
   self.assertEqual(checks['already-done']['status'],'passed')
if __name__=='__main__':unittest.main()
