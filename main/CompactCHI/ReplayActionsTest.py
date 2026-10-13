"""Host intent-format and serial-qualification checks; no DUT execution."""
import copy
from pathlib import Path
import runpy
import tempfile
import unittest

replay = runpy.run_path(str(Path(__file__).with_name("replay-actions.py")))


class ReplayActionsTest(unittest.TestCase):
    def setUp(self):
        self.actions=[dict(cycle=20,node=0,kind=11,address=4096,txn=37,trace_tag=True,size=6,
                           byte_enable=(1<<64)-1,data=bytes(range(64)).hex()),
                      dict(cycle=30,node=4,kind=10,address=262144,txn=38,trace_tag=False,size=3,
                           byte_enable=165,data=(bytes(range(64))[::-1]).hex())]
        self.fields=("mshrs","responses","writebacks","requests","queues","pipeline",
                     "serializers","controllers","retryOwners","credits")
        self.resources=[dict(cycle=a["cycle"],ready=True,**{k:0 for k in self.fields}) for a in self.actions]

    def test_complete_fields_roundtrip(self):
        result=replay["decode"](replay["encode"](self.actions))
        self.assertEqual(result,[replay["validate"](a) for a in self.actions])
        self.assertEqual(result[1]["byte_enable"],165)
        upper=copy.deepcopy(self.actions); upper[0]["data"]=upper[0]["data"].upper()
        self.assertEqual(replay["encode"](upper),replay["encode"](self.actions))

    def test_unsupported_and_incomplete_fields_rejected(self):
        for field,value in (("kind",12),("txn",256),("trace_tag",2),("size",7),
                            ("node",-1),("address",4097),("byte_enable",1<<64),("data","ab")):
            with self.subTest(field=field):
                changed=copy.deepcopy(self.actions); changed[0][field]=value
                with self.assertRaises(ValueError): replay["encode"](changed)
        for actions in ([],self.actions*129):
            with self.assertRaises(ValueError): replay["encode"](actions)

    def test_version_and_numeric_fields_rejected(self):
        text=replay["encode"](self.actions)
        for changed in (text.replace("V1","V2"),text.replace("0 11 ","-1 11 "),text+"bad row\n"):
            with self.assertRaises(ValueError): replay["decode"](changed)

    def test_illegal_and_startup_are_not_protocol_failures(self):
        for text in ("Protocol cycle failed: node rejected illegal stimulus action",
                     "Protocol cycle failed: action selected an unconfigured node", "invalid replay header"):
            self.assertEqual(replay["classify_failure"](text)[0],"input-or-runtime-failed")
        self.assertEqual(replay["classify_failure"]("Protocol cycle failed: upstream data mismatch"),
                         ("protocol-failed","upstream data mismatch"))

    def test_observed_identity_and_resource_proof(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); (root/"replay-actions.txt").write_text(replay["encode"](self.actions))
            self.assertTrue(replay["check"](root,self.actions,self.resources)["completeIntentMatched"])
            for changed in (self.actions[:-1],list(reversed(self.actions))):
                with self.assertRaises(AssertionError): replay["check"](root,changed,self.resources)
            for field,value in (("node",2),("data","ff"*64),("byte_enable",1),("txn",55)):
                changed=copy.deepcopy(self.actions); changed[0][field]=value
                with self.assertRaises(AssertionError): replay["check"](root,changed,self.resources)
            busy=copy.deepcopy(self.resources); busy[0]["controllers"]=1
            with self.assertRaises(AssertionError): replay["check"](root,self.actions,busy)


if __name__=="__main__":
    unittest.main()
