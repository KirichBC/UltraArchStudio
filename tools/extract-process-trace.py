"""Keep only Ultra7z child lifetime events from a tracerpt XML dump."""
import argparse, json
import xml.etree.ElementTree as ET
from pathlib import Path
p = argparse.ArgumentParser()
p.add_argument('xml', type=Path)
p.add_argument('--output', type=Path, required=True)
args = p.parse_args()
count, commands = 0, 0
with args.output.open('w', encoding='utf-8') as output:
    for _, event in ET.iterparse(args.xml, events=('end',)):
        if event.tag.split('}')[-1] != 'Event':
            continue
        fields = {}
        for el in event.iter():
            name = el.tag.split('}')[-1]
            if name == 'Data':
                fields[el.attrib.get('Name', 'unnamed')] = el.text
            elif name == 'TimeCreated':
                fields['time'] = el.attrib.get('SystemTime')
            elif name == 'EventID':
                fields['eventId'] = el.text
            elif name == 'Provider' and el.attrib:
                fields['provider'] = el.attrib
        ns = {'e': 'http://schemas.microsoft.com/win/2004/08/events/event'}
        fields['opcode'] = event.findtext('e:System/e:Opcode', namespaces=ns)
        fields['opcodeName'] = event.findtext('e:RenderingInfo/e:Opcode', namespaces=ns)
        serialized = json.dumps(fields, ensure_ascii=False)
        if '7z.exe' in serialized.lower() or 'ultra7z archive optimizer.exe' in serialized.lower():
            output.write(serialized + '\n')
            count += 1
            commands += int(any('command' in key.lower() for key in fields))
        event.clear()
summary = {'matchedEvents':count, 'eventsWithCommandField':commands,
           'scope':'Events whose decoded data mentions 7z.exe or Ultra7z executable; ETW started after optimization began'}
args.output.with_suffix('.summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
print(json.dumps(summary))
