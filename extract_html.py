import re

with open('anenji_esp32_monitor.ino', 'r', encoding='utf-8') as f:
    content = f.read()

match = re.search(r'const char index_html\[\] PROGMEM = R"rawliteral\((.*?)\)rawliteral\";', content, re.DOTALL)
if match:
    html = match.group(1)
    with open('preview.html', 'w', encoding='utf-8') as f:
        f.write(html)
    print('HTML extracted to preview.html')
    print(f'Length: {len(html)} chars')
else:
    print('Not found')