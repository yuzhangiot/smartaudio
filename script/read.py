import httplib, urllib, json, pprint
from wand.image import Image
import numpy as np
from pylab import *
# from pprint import pprint

def readdata(num):

	params = urllib.urlencode({'results' : num,'key':'5PTJZFXQ6SWD32PR'})
	headers = {"Content-type": "application/x-www-form-urlencoded","Accept":"text/plain"}
	conn = httplib.HTTPConnection("192.168.10.88:3000")
	conn.request("GET", "/channels/1/fields/1.json", params, headers)
	response = conn.getresponse()
	print response.status, response.reason
	data = response.read()
	conn.close()

	databuffer = []
	mydata = data.decode('utf-8')
	decoded_data = json.loads(mydata)

	for feed in decoded_data["feeds"]:
		databuffer.append(feed["field1"])

	return databuffer

def writedatatofile(filename, databuffer):
	
	f = open("data/"+filename, "w")
	
	# databuffer = readdata(50)
	# encoded_data = json.dumps(mydata)
	
	# pprint.pprint(mydata)
	# pprint.pprint(encoded_data)
	# pprint.pprint(decoded_data)
	# print type(mydata)
	# print type(encoded_data)
	# print type(decoded_data)

	# for dd in decoded_data:
		# print dd
		# for d in decoded_data[dd]:
		# 	print "-----" + str(d)
			# print d

	# print decoded_data["feeds"][1]["field1"]
	
	# pprint.pprint(databuffer)
	# print type(databuffer)
	for str in databuffer:
		f.write("%s\n" % str)

def seperatedata():
	databuffer = readdata(280)
	lists = [[] for i in range(6)]
	newflag = False
	mycount = 0
	for num in databuffer:
		if (num == '0'):
			newflag = True
			continue
		if (newflag == True):
			newflag = False
			mycount += 1
		lists[mycount].append(num)
	print lists[1][55]
	# print lists
	return lists

def drawPicSeven():
	figure(figsize=(8,6), dpi=80)

	subplot(1,1,1)

	X = np.linspace(1, 50, 50,endpoint=True)
	# C,S = np.cos(X), np.sin(X)
	databuffer = readdata(50)

	plot(X, databuffer, color="blue", linewidth=1.5, linestyle="-", label="noise only")
	# plot(X, pi, color="green", linewidth=1.5, linestyle=":", label="RaspberryPi")
	# plot(X, cool4, color="red", linewidth=1.5, linestyle="--", label="Linux Server")

	# plot(X, S, color="green", linewidth=1.0, linestyle="-")

	xlim(0,50.0)

	xticks(np.linspace(0,50,6,endpoint=True))

	ylim(0,150)

	yticks(np.linspace(0.000,150,11,endpoint=True))
	legend(loc='upper left')

	show()

def drawPicEight():
	figure(figsize=(8,6), dpi=80)

	subplot(1,1,1)

	X = np.linspace(1, 50, 50,endpoint=True)
	# C,S = np.cos(X), np.sin(X)
	with open('data/figure8_1') as f:
		num01 = f.read().splitlines()
	with open('data/figure8_2') as f:
		num02 = f.read().splitlines()
	with open('data/figure8_3') as f:
		num03 = f.read().splitlines()
	with open('data/figure8_4') as f:
		num04 = f.read().splitlines()
	# with open('data/figure8_5') as f:
	# 	num05 = f.read().splitlines()


	plot(X, num01[3:53], color="blue", linewidth=1.5, linestyle="-", label="first test")
	plot(X, num02[1:51], color="green", linewidth=1.5, linestyle="-", label="second test")
	plot(X, num03[2:52], color="red", linewidth=1.5, linestyle="-", label="third test")
	plot(X, num04[3:53], color="orange", linewidth=1.5, linestyle="-", label="forth test")
	# plot(X, num05[3:53], color="black", linewidth=1.5, linestyle="-", label="fifth test")
	# plot(X, pi, color="green", linewidth=1.5, linestyle=":", label="RaspberryPi")
	# plot(X, cool4, color="red", linewidth=1.5, linestyle="--", label="Linux Server")

	# plot(X, S, color="green", linewidth=1.0, linestyle="-")

	xlim(0,50.0)

	xticks(np.linspace(0,50,6,endpoint=True))

	ylim(0,60)

	yticks(np.linspace(0.000,60,7,endpoint=True))
	legend(loc='upper left')

	show()

# writedatatofile("figure7")
# drawPicSeven()
# mylist = seperatedata()
# writedatatofile("figure8_1", mylist[1][0:55])
# writedatatofile("figure8_2", mylist[1][56:])
# writedatatofile("figure8_3", mylist[2])
# writedatatofile("figure8_4", mylist[3])
# writedatatofile("figure8_5", mylist[4])
# drawPicEight()




