# -*- coding: utf-8 -*-
# Test fixture: a PyPlug whose inner chain is deep (GroupInput -> deep gain -> GroupOutput).
# Loaded by Tests/PyPlugInstantiate_Test.cpp, which adds this directory to the plug-in path.

import NatronEngine
import sys

def getPluginID():
    return "test.natron.pyplug.DeepGainGroup"

def getLabel():
    return "TestDeepGainGroup"

def getVersion():
    return 1

def getGrouping():
    return "Test"

def getPluginDescription():
    return "Deep gain inside a group, to check that a PyPlug's kind follows what feeds its GroupOutput."

def createInstance(app,group):
    # Start of node "Gain1"
    lastNode = app.createNode("test.natron.built-in.DeepRenderGain", -1, group)
    lastNode.setScriptName("Gain1")
    lastNode.setLabel("Gain1")
    groupGain1 = lastNode

    del lastNode
    # End of node "Gain1"

    # Start of node "Output1"
    lastNode = app.createNode("fr.inria.built-in.Output", 1, group)
    lastNode.setLabel("Output1")
    groupOutput1 = lastNode

    del lastNode
    # End of node "Output1"

    # Start of node "Source"
    lastNode = app.createNode("fr.inria.built-in.Input", 1, group)
    lastNode.setScriptName("Source")
    lastNode.setLabel("Source")
    groupSource = lastNode

    del lastNode
    # End of node "Source"

    # Now that all nodes are created we can connect them together
    groupGain1.connectInput(0, groupSource)
    groupOutput1.connectInput(0, groupGain1)
