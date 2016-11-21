import sys
sys.path.append("Web/Server")
sys.path.append('Web/Client/Scenarios/final')
from klampt import *
from common import *
import time
import random
import traceback
import sensor
import kviz
import math

class EventC:
    """This class does the event logic"""
    def __init__(self,sim):
        self.difficulty = stub.difficulty
        self.score = 0
        self.forfeit = False
        
        self.lastpenaltytime = 0
        self.tcontroller = 0

        self.maxTries = 10
        self.ball = 0
        self.lasttouchtime = None
        self.endtime = 60
        if self.difficulty == 'medium':
            self.endtime = 50
        elif self.difficulty == 'hard':
            self.endtime = 40

        self.initialStates = None

        #activate collision feedback
        robot = sim.world.robot(0)
        for i in range(robot.numLinks()):
            for j in range(sim.world.numTerrains()):
                sim.enableContactFeedback(robot.link(i).getID(),sim.world.terrain(j).getID())
            #robot ball feedback
            sim.enableContactFeedback(robot.link(i).getID(),sim.world.rigidObject(0).getID())
        
    def mark_controller_time(self,tcontroller):
        self.tcontroller += tcontroller
        
    def update(self,dt,sim):
        if self.forfeit: return
        if self.initialStates == None:
            self.initialStates = [sim.body(sim.world.rigidObject(obj)).getTransform() for obj in range(sim.world.numRigidObjects())]
        for obj in range(1,sim.world.numRigidObjects()):
            sim.body(sim.world.rigidObject(obj)).enable(False) 
        t = sim.getTime()
        if t > self.lastpenaltytime + 1.0:
            if self.tcontroller > 5:
                print "Event supervisor: Took too long to compute controls"
                print "  Result: forfeit"
                self.score -= 5
                self.forfeit = True
            elif self.tcontroller > 1:
                print "Event supervisor: Took too long to compute controls"
                print "  Time",self.tcontroller,"over the last 1s"
                print "  Penalty: 1"
                self.score -= 1
            self.tcontroller = 0
            self.lastpenaltytime = t
        #check joint limits, velocity limits, and torque limits
        qrobot = sim.getActualConfig(0)
        vrobot = sim.getActualVelocity(0)
        trobot = sim.getActualTorques(0)
        qmin,qmax = sim.world.robot(0).getJointLimits()
        vmax = sim.world.robot(0).getVelocityLimits()
        tmax = sim.world.robot(0).getTorqueLimits()
        for i in range(7):
            if qrobot[i] < qmin[i] or qrobot[i] > qmax[i]:
                print "Event supervisor: Out of joint limits"
                self.score -= dt*10
                break
        for i in range(1,7):
            if abs(vrobot[i]) > vmax[i]:
                print "Event supervisor: Out of velocity limits"
                print vrobot,vmax
                self.score -= dt*10
                break
        for i in range(6):
            if abs(trobot[i]) > tmax[i+1]:
                print "Event supervisor: Out of torque limits"
                print trobot,tmax
                self.score -= dt*10
                break
        #check collisions between robot and terrain
        if self.inContact(sim):
            print "Event supervisor: in contact with terrain"
            self.score -= dt*30
            
        #do ball kicking logic
        self.doGameLogic(sim)
        return
    
    def doGameLogic(self,sim):
        t = sim.getTime()

        #determine if ball touched; reset after 3 seconds
        robot = sim.world.robot(0)
        for i in range(robot.numLinks()):
            if sim.hadContact(robot.link(i).getID(),sim.world.rigidObject(0).getID()):
                self.lasttouchtime = t

        goalcenter = (3.5,0,0.5)
        goaldims = (0.5,2,1)
        goalmin = vectorops.madd(goalcenter,goaldims,-0.5)
        goalmax = vectorops.madd(goalcenter,goaldims,0.5)
        obj = sim.world.rigidObject(0)
        ballbody = sim.body(obj)
        ballbody.enable(True)
        respawn = False
        if self.ball < self.maxTries:
            Tb = ballbody.getTransform()
            Rb,tb = Tb
            if all(tb[i] > goalmin[i] and tb[i] < goalmax[i] for i in range(3)):
                print "Event supervisor: Ball",i,"scored, adding 10 points"
                self.score += 10
                respawn = True
        
        if self.lasttouchtime != None and t > self.lasttouchtime + 3.0:
            print "Event supervisor: Ball",i,"passed 3 seconds, respawning"
            respawn = True

        if ballbody.getTransform()[1][2] < 0:
            #fallen off the edge
            print "Event supervisor: Ball",i,"fell off the playing field, respawning"
            respawn = True

        if respawn:
            ballbody.setTransform(self.initialStates[0][0],self.initialStates[0][1])
            ballbody.setVelocity([0]*3,[0]*3)
            self.lasttouchtime = None
            self.ball += 1

        #drive obstacles
        if not hasattr(self,'phase_shifts'):
            self.phase_shifts = [random.uniform(0,math.pi*2) for i in range(sim.world.numRigidObjects())]
        for i in range(1,sim.world.numRigidObjects()):
            Tx = self.initialStates[i]
            period = 5+i*2
            amplitude = 1.2
            if self.difficulty == "medium":
                period = 4+i*2
                amplitude = 0.9
            elif self.difficulty == "hard":
                period = 5+i*1.5
                amplitude = 0.6
            phase = i + self.phase_shifts[i]
            delta = amplitude*math.sin((t+phase)/period*math.pi*2)
            vdelta = amplitude*math.cos((t+phase)/period*math.pi*2)*math.pi*2/period
            Tnew = (Tx[0],vectorops.add(Tx[1],[0,delta,0]))
            sim.body(sim.world.rigidObject(i)).setTransform(*Tnew)
            sim.body(sim.world.rigidObject(i)).setVelocity([0,0,0],[0,vdelta,0])
    
    def inContact(self,sim):
        """Returns true if the robot touches the environment"""
        robot = sim.world.robot(0)
        for i in range(robot.numLinks()):
            for j in range(sim.world.numTerrains()):
                if sim.hadContact(robot.link(i).getID(),sim.world.terrain(j).getID()):
                    return True
        return False

class GLTest:
    def __init__(self,simWorld,planningWorld):
        self.simWorld = simWorld
        self.planningWorld = planningWorld
        self.sim = Simulator(self.simWorld)
        self.event = EventC(self.sim)
        #set up sensors
        self.sensors = dict()
        cameraRot = [0,-1,0,0,0,-1,1,0,0]
        #on ground near robot, pointing to the right
        Tsensor = (cameraRot,[-1.5,-0.5,0.25])
        if stub.omniscient_sensor:
            self.sensors['omniscient'] = sensor.OmniscientObjectSensor()
        else:
            self.sensors['blobdetector'] = sensor.CameraColorDetectorSensor()
            self.sensors['blobdetector'].Tsensor = Tsensor
        self.controller = stub.MyController(self.planningWorld,self.sim.controller(0))
        
        #set up camera to get a better vantage point
        #self.camera.dist = 12
        #self.camera.tgt[2] = -1
        #self.clippingplanes = (0.2,50)
        
        self.dt = 0.02
        self.sim.simulate(0)
        self.simulate = True
        self.finalScore = None
        self.initVis()

    def initVis(self):
        kviz._init(self.simWorld)
        kviz.add_text("time",5,5)
        kviz.add_text("score",5,10)
        kviz.add_text("final",5,15)
        if 'blobdetector' in self.sensors:
            gldraw.xform_widget(self.sensors['blobdetector'].Tsensor,0.1,0.01,fancy=True)
        self.updateVis()

    def updateVis(self):
        kviz.update_text("time","Time: "+str(self.sim.getTime()))
        kviz.update_text("score","Score: "+str(self.event.score))
        if self.finalScore != None:
            kviz.update_text("final","Final score: "+str(self.finalScore))

    def control_loop(self):
        readings = dict()
        for n,s in self.sensors.iteritems():
            readings[n] = s.emulate(self.sim)
        try:
            self.controller.loop(self.dt,self.sim.controller(0),readings)
        except Exception as e:
            print "Exception called during controller.loop:"
            traceback.print_exc()

    def step(self):
        t0 = time.time()
        self.control_loop()
        tcontroller = time.time()-t0
        self.event.mark_controller_time(tcontroller)
        
        self.sim.simulate(self.dt)
        self.event.update(self.dt,self.sim)
        if self.finalScore == None and self.sim.getTime() >= self.event.endtime:
            self.finalScore = self.event.score
        self.updateVis()

program,world,world2 = None,None,None

def boilerplate_start():
    global program,world,world2
    world = WorldModel()
    world2 = WorldModel()
    fn = "Web/Client/Scenarios/final/finalC.xml"
    res = world.readFile(fn)
    if not res:
        raise RuntimeError("Unable to load world "+fn)
    res = world2.readFile(fn)
    for i in range(world.numRigidObjects()):
        world.rigidObject(i).appearance().setColor(*sensor.objectColors[i%len(sensor.objectColors)])
        world2.rigidObject(i).appearance().setColor(*sensor.objectColors[i%len(sensor.objectColors)])
    program = GLTest(world,world2)
    random.seed(stub.random_seed)

def boilerplate_advance():
    global program
    program.step()
    program.sim.updateWorld()

