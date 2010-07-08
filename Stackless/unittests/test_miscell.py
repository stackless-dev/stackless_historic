# import common

import pickle
import unittest
import stackless

def in_psyco():
    try:
        return __in_psyco__
    except NameError:
        return False
    
def is_soft():
    softswitch = stackless.enable_softswitch(0)
    stackless.enable_softswitch(softswitch)
    return softswitch and not in_psyco()

def runtask():
    x = 0
    # evoke pickling of an xrange object
    dummy = xrange(10)
    for ii in xrange(1000):
        x += 1


class TestWatchdog(unittest.TestCase):

    def lifecycle(self, t):
        # Initial state - unrun
        self.assert_(t.alive)
        self.assert_(t.scheduled)
        self.assertEquals(t.recursion_depth, 0)
        # allow hard switching
        t.set_ignore_nesting(1)
        
        softSwitching = is_soft()
        
        # Run a little
        res = stackless.run(10)
        self.assertEquals(t, res)
        self.assert_(t.alive)
        self.assert_(t.paused)
        self.assertFalse(t.scheduled)
        self.assertEquals(t.recursion_depth, softSwitching and 1 or 2)

        # Push back onto queue
        t.insert()
        self.assertFalse(t.paused)
        self.assert_(t.scheduled)
        
        # Run to completion
        stackless.run()
        self.assertFalse(t.alive)
        self.assertFalse(t.scheduled)
        self.assertEquals(t.recursion_depth, 0)
        

    def test_aliveness1(self):
        """ Test flags after being run. """
        t = stackless.tasklet(runtask)()
        self.lifecycle(t)

    def test_aliveness2(self):
        """ Same as 1, but with a pickled unrun tasklet. """
        import pickle
        t = stackless.tasklet(runtask)()
        t_new = pickle.loads(pickle.dumps((t)))
        t.remove()
        t_new.insert()
        self.lifecycle(t_new)
   
    def test_aliveness3(self):
        """ Same as 1, but with a pickled run(slightly) tasklet. """

        t = stackless.tasklet(runtask)()
        t.set_ignore_nesting(1)

        # Initial state - unrun
        self.assert_(t.alive)
        self.assert_(t.scheduled)
        self.assertEquals(t.recursion_depth, 0)

        softSwitching = is_soft()

        # Run a little
        res = stackless.run(100)
        
        self.assertEquals(t, res)
        self.assert_(t.alive)
        self.assert_(t.paused)
        self.assertFalse(t.scheduled)
        self.assertEquals(t.recursion_depth, softSwitching and 1 or 2)        
        
        # Now save & load
        dumped = pickle.dumps(t)
        t_new = pickle.loads(dumped)        

        # Remove and insert & swap names around a bit
        t.remove()
        t = t_new
        del t_new
        t.insert()

        self.assert_(t.alive)
        self.assertFalse(t.paused)
        self.assert_(t.scheduled)
        self.assertEquals(t.recursion_depth, 1)
        
        # Run to completion
        if is_soft():
            stackless.run()
        else:
            t.kill()
        self.assertFalse(t.alive)
        self.assertFalse(t.scheduled)
        self.assertEquals(t.recursion_depth, 0)
    
#///////////////////////////////////////////////////////////////////////////////

if __name__ == '__main__':
    import sys
    if not sys.argv[1:]:
        sys.argv.append('-v')

    unittest.main()
