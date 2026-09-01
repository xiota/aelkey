data_dir = get_option('datadir') / 'aelkey' / 'system'

udev_files = [
  '72-aelkey-general-group.rules',
  '72-aelkey-general-uaccess.rules',
  '72-playstation-disable-touchpad.rules',
]

install_data(
  udev_files,
  'sysusers-aelkey.conf',
  install_dir: data_dir,
)
