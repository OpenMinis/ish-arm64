python3 -c "
from bs4 import BeautifulSoup
s = BeautifulSoup(open('/tmp/aojit/py/page.html').read(), 'html.parser')
links = [a['href'] for a in s.find_all('a')]
bold = len(s.select('div.c3 b'))
print(len(links), bold, s.title.string)
"
