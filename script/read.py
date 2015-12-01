import httplib, urllib, json, pprint
from wand.image import Image
import numpy as np
from pylab import *
# from pprint import pprint

def readdata():

	params = urllib.urlencode({'results' : 50,'key':'5PTJZFXQ6SWD32PR'})
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

def writedatatofile(filename):
	
	f = open("data/"+filename, "w")
	
	databuffer = readdata()
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

def drawPicSeven():
	figure(figsize=(8,6), dpi=80)

	subplot(1,1,1)

	X = np.linspace(1, 50, 50,endpoint=True)
	# C,S = np.cos(X), np.sin(X)
	databuffer = readdata()

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

# writedatatofile("figure7")
drawPicSeven()

