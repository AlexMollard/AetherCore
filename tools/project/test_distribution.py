import copy
import json
from pathlib import Path
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
  required=json.loads((Path(__file__).resolve().parents[1]/'browser-tests/required-cases.json').read_text())
  report={'status':'passed','cases':[{'name':name,'status':'passed'} for name in required['requiredBridgeCases']],'legacy':{'status':'passed','cases':[{'name':name,'status':'passed'} for name in required['legacyCaseNames']]},'cleanup':{'browserClosed':True,'serverClosed':True,'childClosed':True}}
  self.assertTrue(distribution.browser_passed(report))
  for group in ('cases','legacy'):
   for mutation in ('missing','duplicate','renamed','skipped','software'):
    broken=copy.deepcopy(report)
    cases=broken['cases'] if group=='cases' else broken['legacy']['cases']
    if mutation=='missing':cases.pop()
    elif mutation=='duplicate':cases[-1]=copy.deepcopy(cases[0])
    elif mutation=='renamed':cases[-1]['name']='unrelated-passing-case'
    elif mutation=='skipped':cases[-1]['status']='skipped'
    else:cases[-1]['hardware']=False
    with self.subTest(group=group,mutation=mutation):
     self.assertFalse(distribution.browser_passed(broken))
 def test_executed_but_unfinished_checks_are_not_called_unexecuted(self):
  for cancelled in (False,True):
   checks={'compile':{'status':'running'},'already-done':{'status':'passed'}}
   distribution.finish_unvalidated(checks,cancelled)
   self.assertEqual(checks['compile']['status'],'cancelled' if cancelled else 'failed')
   self.assertEqual(checks['already-done']['status'],'passed')
if __name__=='__main__':unittest.main()
