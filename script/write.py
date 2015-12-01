import httplib, urllib

params = urllib.urlencode({'field1': 999, 'key':'5PTJZFXQ6SWD32PR'})
headers = {"Content-type": "application/x-www-form-urlencoded","Accept":"text/plain"}
conn = httplib.HTTPConnection("192.168.10.88:3000")
conn.request("POST", "/update", params, headers)
response = conn.getresponse()
print response.status, response.reason
data = response.read()
conn.close()